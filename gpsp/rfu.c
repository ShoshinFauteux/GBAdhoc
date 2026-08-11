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

#include "common.h"

// Debug print logic:
#ifdef RFU_DEBUG
  #define RFU_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
  #define RFU_DEBUG_LOG(...)
#endif

// Config knobs, update with care.
/* Depth of the per-direction packet queues between netpacket delivery and
 * the GBA-side RFU polls.  The original depth of 4 assumes an essentially
 * zero-latency transport (frames arrive one per emulated frame, as on real
 * radio slot timing); any transport with real latency (ARQ retransmits,
 * jittered links) delivers legitimate bursts deeper than 4 and the excess
 * was silently discarded � including one-shot game command packets (e.g.
 * Union Room trade requests), which the games never retransmit.  16 absorbs
 * ~250 ms of clumped deliveries at the link's steady ~2 frames/frame. */
/* Per-peer RFU receive queue depth (ADR-0044).
 *
 * 4 originally; ADR-0011 raised it to 16 after dropped clumps wedged a trade.
 * The field then produced ~55 `rfu_qdrop side=host_rx` in a single session,
 * during the trade animation, while the client's transport was backed up
 * (srtt 344ms, rto pinned at 800ms, retx 15%, txq_hi 132).  A reliable
 * transport that has stalled delivers in a BURST once it catches up, and 16
 * slots cannot absorb one.  Every dropped entry is RFU payload the games have
 * no way to recover -- which is exactly how a trade wedges.
 *
 * ADR-0041 cautioned against raising this again, on the theory that a deeper
 * queue lets bigger clumps reach the game and overflow ITS 32-slot queue.
 * That theory was tested on hardware in ADR-0042 and failed: capping delivery
 * did not prevent the crash and made the game poll HARDER (8 -> 24 per frame).
 * The caution was reasonable when written and the evidence has since moved.
 *
 * Note the depth does not decide how much the game is handed per frame -- it
 * pulls one packet per RFU_CMD_RECV_DATA at its own rate (measured peak 8/frame
 * in the field).  Depth only decides how much we can HOLD without losing it.
 *
 * Cost: ~10 KB of BSS against ~380 KB free in session.  RFU_TR_QHI reports the
 * deepest the queue actually gets, so the next person does not have to guess
 * whether 64 was enough either. */
#define RFU_PKT_QUEUE                  64
#define BCST_ANNOUNCE_VB               30   // Send broadcast twice per second.
#define MAX_RFU_PEERS  MAX_RFU_NETPLAYERS   // Do not allow more than 32 peers.

#define RFU_DEF_TIMEOUT              32   // Expressed as frames (~533ms)
#define RFU_DEF_RTXMAX                4   // Up to 4 transmissions per send


// Unpacks big endian integers used for signaling
static inline u32 upack32(const u8 *ptr) {
  return ptr[3] | (ptr[2] << 8) | (ptr[1] << 16) | (ptr[0] << 24);
}

// Unpacks payload data, which is little-endian
static inline u32 leupack32(const u8 *ptr) {
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

// The following commands, names and bit fields are not a 100% known.
// Most of them is guessed via reverse engineering the adapter and/or
// games that use it. Please take it with a grain of salt.
// You might wanna check:
// https://github.com/afska/gba-link-connection/
// https://blog.kuiper.dev/gba-wireless-adapter

// Frontend notification hook, fired when a game completes the adapter
// login handshake (i.e. the wireless adapter is actively in use).  Weak
// no-op by default so the libretro build is unaffected; the PSP native
// frontend overrides it for the silent-wireless per-game builds
// (docs/VARIANTS.md, ADR-0013).  Called from the emulation thread; must
// be cheap and idempotent (games re-run the handshake on every adapter
// reset).
void gpsp_rfu_activated_hook(void);
__attribute__((weak)) void gpsp_rfu_activated_hook(void) {}

// Symmetric notification: the emulated RFU link went DOWN, and why.  Exists
// so a frontend log can distinguish "the game ended the wireless session"
// (the player walked out of the Union Room) from "the frontend tore the
// transport down" — the two are indistinguishable today, which cost a
// field-debug round trip.  Same contract as the hook above: weak no-op,
// called from the emulation thread, must be cheap and idempotent.
//   reason 1 = local RFU_CMD_DISCONNECT (this console ended it)
//          2 = peer sent NET_RFU_DISCONNECT
//          3 = adapter reset (GPIO)
//          4 = client timed out (host side)
#define RFU_DOWN_LOCAL    1
#define RFU_DOWN_PEER     2
#define RFU_DOWN_RESET    3
#define RFU_DOWN_TIMEOUT  4
void gpsp_rfu_link_down_hook(unsigned reason, unsigned slot);
__attribute__((weak)) void gpsp_rfu_link_down_hook(unsigned reason,
                                                   unsigned slot)
{ (void)reason; (void)slot; }

/* Diagnostic trace of the emulated adapter's command and state stream
 * (phase5j).  The four link_down reasons above cover only the paths that
 * end a *connection*; they say nothing about the far more consequential
 * event, which is this adapter answering a command with an ERROR.  Gen-3's
 * librfu turns any non-zero REQ result into LMAN_MSG_REQ_API_ERROR, which
 * pokeemerald maps to RFU_STATUS_FATAL_ERROR -> CB2_LinkError with
 * `disconnected = FALSE` -> gWirelessCommType = 3 -> the power-off-only
 * error screen (src/link.c:1589-1610, src/link_rfu_2.c:1995).  A
 * *connection* error, by contrast, yields RFU_STATUS_CONNECTION_ERROR and
 * the recoverable "press A" dialog.  So every `return -1` below is a
 * candidate generator of the fatal screen and none of them was visible.
 *
 * Same contract as the hooks above: weak no-op, emulation thread, cheap.
 *   ev 1 CMDERR  a=command, b=rfu_state   (adapter answered ERROR)
 *      2 STATE   a=new rfu_state, b=cause (see RFU_TRC_* below)
 *      3 UNKCMD  a=command, b=rfu_state   (fell through to default:)
 *      4 QDROP   a=0 client-rx/1 host-rx, b=slot (packet queue was full)
 *      5 CMD     a=command, b=rfu_state   (structural commands only —
 *                the per-frame chatty ones are filtered out)
 *      6 LOGIN   a=0 reset entered / 1 handshake begun / 2 login complete,
 *                b=SPI words spent in the previous phase.
 *
 * Why 6 exists.  librfu's adapter re-init is `AgbRFU_SoftReset()` (the GPIO
 * pulse that lands here as rfu_reset) immediately followed by
 * `AgbRFU_checkID()`; if that ID check fails, the link manager raises
 * LMAN_MSG_RFU_FATAL_ERROR -> RFU_STATUS_FATAL_ERROR, i.e. the SAME
 * unrecoverable screen, with no packet ever being lost.  That handshake is
 * a timed SPI conversation, so it is a live suspect on a console whose
 * frame loop still spikes to tens of ms, and it was completely unobservable.
 * The rig pulses this path ~28 times in a single Union Room session, so it
 * is not a rare corner.
 *
 * Why 7 exists — the leading hypothesis, and this is the number that
 * settles it.  gen-3 buffers link frames the adapter hands it in
 * `gRfu.recvQueue`: 32 slots, filled once per MSC callback but drained
 * exactly ONCE PER GAME FRAME (pokeemerald link_rfu_2.c:590 vs :937), and
 * `full` is a LATCH that no dequeue ever clears (link_rfu_3.c:391-394,
 * :437).  When it latches, RfuCheckErrorStatus raises
 * RFU_STATUS_FATAL_ERROR — NOT CONNECTION_ERROR — which is precisely the
 * unrecoverable screen (link_rfu_2.c:1999-2005).
 *
 * On real radio the adapter delivers one frame per frame, so that queue
 * cannot outrun its drain.  Over a transport with tens of ms of RTT it can:
 * deliveries arrive CLUMPED, and ADR-0011 deliberately widened rfu.c's own
 * queue from 4 to 16 to stop dropping those clumps — which means up to 16
 * of them now reach the game in one emulated frame instead of being
 * discarded here.  ADR-0011 was right that dropping them wedged the trade;
 * the open question it left is whether the clumps now overflow the GAME's
 * queue instead, and nothing has ever measured that.
 *
 * RFU_TR_RXBURST reports a new per-frame high-water in RFU_CMD_RECV_DATA
 * commands served.  Steady state is 1.  Anything that climbs toward 32 in a
 * single frame is the mechanism, and it costs one counter to find out. */
#define RFU_TR_CMDERR     1
#define RFU_TR_STATE      2
#define RFU_TR_UNKCMD     3
#define RFU_TR_QDROP      4
#define RFU_TR_CMD        5
#define RFU_TR_LOGIN      6
#define RFU_TR_RXBURST    7
#define RFU_TR_RXHOLD     8
#define RFU_TR_QHI        9   /* deepest the RFU rx queue has been (ADR-0044) */
#define RFU_TR_ANSWER    10   /* ADR-0055: client answer latency */
#define RFU_TR_ANSSTAT   11   /* ADR-0058: periodic answer-latency census */
/* ADR-0059: THE ADAPTER'S OWN DEADLINES, finally observable.
 *
 * Every timeout this file enforces is counted in EMULATED CPU CYCLES while the
 * radio underneath is wall clock, so the budget the game actually gets is
 * `T / session_fps` seconds and shrinks as we raise the session rate.  That is
 * the central inference of CLIFF-FINDINGS and it was never a measurement,
 * because neither of the two deadlines had a trace hook and `rfu_timeout`
 * itself is overwritten by the game via RFU_CMD_SYSCFG and never logged.
 *
 *   RFU_TR_TIMEO   the CLIENT ran out of time -> RFU_CMD_RESP_TIMEO.
 *                  a = cumulative count, b = the live rfu_timeout in frames.
 *   RFU_TR_SYSCFG  the game just set rfu_timeout / rfu_rtx_max.  Once per run.
 *                  a = rfu_timeout (frames), b = rfu_rtx_max.
 *   RFU_TR_NORESP  the HOST's rfu_resp_timeout (rtx_max/6 frames -- 11 ms at
 *                  59.73 fps against a 55 ms radio) expired and we handed the
 *                  game a synthetic "clients did not answer".
 *                  a = cumulative count / 16, b = rfu_rtx_max.
 *
 * Emitted at most ONCE PER EMULATED FRAME each, from the per-frame block, and
 * only when the count moved.  The trace ring is 64 entries and already loses
 * events in busy arms (CLIFF §8.2); an unthrottled hook on a timeout that may
 * fire many times per frame would destroy the other measurements. */
#define RFU_TR_TIMEO     12
#define RFU_TR_SYSCFG    13
#define RFU_TR_NORESP    14
/* ADR-0072: THE TWO NUMBERS THAT DECIDE WHETHER THE INPUT BUG IS THIS BUG.
 *
 * pokeemerald discards the CLIENT's field input on receive-queue depth --
 * `KeyInterCB_SelfIdle` (src/overworld.c:2520) returns
 * LINK_KEY_CODE_HANDLE_RECV_QUEUE and never reaches KeyInterCB_ReadButtons
 * while `GetLinkRecvQueueLength() > 4`.  The queue is drained exactly ONCE PER
 * FRAME (link_rfu_2.c:937) and is child-only -- RfuMain1_Parent never touches
 * it -- so the host structurally cannot gate and the client structurally can.
 * That is the whole host/client asymmetry.
 *
 * RFU_TR_RXBURST already proves the bursts exist (join logs reach 9 against a
 * host's 1-2), but it is a HIGH-WATER: it says a 9 happened, never how often.
 * The fraction of frames over the threshold is what separates "this explains
 * the bug" from "this explains a bad moment once a minute", and nothing has
 * ever measured it.
 *
 *   a = frames in this window whose delivery count exceeded the gate (>4)
 *   b = the longest CONSECUTIVE run of them -- i.e. how many frames in a row
 *       the player's controls were dead, which is what a human actually feels.
 *
 * Denominator is the fixed window below, so two counters give both rates. */
#define RFU_TR_RXGATE    15
/* ADR-0072: the recoverable "press A to return to lobby" screen, which the
 * user hit and which HAS NEVER BEEN OBSERVABLE.  Every hooked error path in
 * this file is the FATAL family (gWirelessCommType=3, exit by power switch);
 * the recoverable variant needs RFU_STATUS_CONNECTION_ERROR, and the only
 * thing here that produces it is the RESP_DISC answer below. */
#define RFU_TR_DISCANS   16
/* ADR-0072: host-packet inter-arrival clumping at the CLIENT, measured at the
 * wire and immune to the poll confound that invalidated RFU_TR_RXBURST. */
#define RFU_TR_ARRIVAL   17
/* ADR-0074: how many host packets were still UNDELIVERED when the peer's
 * disconnect landed -- i.e. how much of the exit negotiation the old code
 * destroyed.  a = queued, b = whether deferral was enabled. */
#define RFU_TR_DISCQ     18
/* ADR-0075: the frame-pace census.  a = frames in the window on which the
 * pace gate deferred at least one eligible packet to a later frame; b = the
 * deepest our client queue got on those frames.  Emitted every window while
 * `rfu_frame_pace` is on, INCLUDING n=0 -- ADR-0058's lesson: a probe whose
 * silence is indistinguishable from a dead instrument is not evidence. */
#define RFU_TR_PACE      19
/* ADR-0072 fix: the game's recvQueue-depth PEAK, emitted on its own event so it
 * survives the ring's 12-bit fields (the old RFU_TR_RXGATE packed it at <<16 and
 * the ring masked it to zero -- trap 6, "peak always 0", the unanswered
 * corrected-rxgate).  a = rfu_gq_peak (the deepest the game's 32-slot recvQueue
 * modelled this window; >4 trips Gate B, 32 latches FATAL), b = frames over the
 * gate this window.  Both fit 12 bits (window is 600). */
#define RFU_TR_GQPEAK    20
/* Arrivals per census.  256 is a few seconds of live trade traffic and keeps
 * the ratio meaningful without flooding the 64-entry trace ring. */
#define RFU_ARRIVAL_CENSUS_N 256
/* ADR-0074: upper bound on the deferral, in emulated frames.  The game drains
 * one packet per frame, so 64 covers a queue at its RFU_PKT_QUEUE ceiling with
 * room to spare; past that something is wrong and the old behaviour is the
 * safer failure. */
#define RFU_DISC_DRAIN_MAX_FRAMES 64

/* Frames per RFU_TR_RXGATE window.  600 = ~10 s at 59.73, matching the other
 * periodic censuses so the lines interleave readably in a log. */
#define RFU_RXGATE_FRAMES 600
/* The game's threshold, from the decomp.  Named rather than inlined so the
 * instrument and the explanation cannot drift apart. */
#define RFU_GAME_RECVQ_GATE 4
/* causes for RFU_TR_STATE */
#define RFU_TRC_HOST_START    1
#define RFU_TRC_HOST_STOP     2
#define RFU_TRC_CONNECT       3
#define RFU_TRC_CONN_ACK      4
#define RFU_TRC_CONN_NACK     5
#define RFU_TRC_CONCOMPL_FAIL 6
#define RFU_TRC_DISC_LOCAL    7
#define RFU_TRC_DISC_PEER     8
#define RFU_TRC_RESET         9
void gpsp_rfu_trace_hook(unsigned ev, unsigned a, unsigned b);
__attribute__((weak)) void gpsp_rfu_trace_hook(unsigned ev, unsigned a,
                                               unsigned b)
{ (void)ev; (void)a; (void)b; }

/* ---- flight recorder (ADR-0043) ----------------------------------------
 *
 * Three outside-in explanations for the exit-Union-Room fatal screen have now
 * been proposed and falsified against hardware: packet delivery bunching (the
 * cap engaged and it still crashed, while making the game poll HARDER), link
 * quality (a clean 63 ms link crashed identically to a 358 ms one), and rate
 * divergence (at 19.98 fps the pair was ~4% apart, which is ~10 frames of
 * drift against a 240-frame timeout — nowhere near enough, and it crashed
 * anyway).
 *
 * Guessing has been tried.  This records instead: every command the game
 * issues, with the adapter state it was issued in and the emulated frame it
 * landed on, in a ring that is continuously overwritten and costs one store.
 *
 * The dump trigger needs no pokeemerald symbol addresses, which matters
 * because those are ROM-revision specific and we would have to trust them.
 * It uses a signature the field has produced in EVERY crash and never
 * otherwise: the adapter being RESET straight out of an active session,
 * which is what librfu does once the game has already declared FATAL.  By
 * then the game has stopped talking, so the ring is frozen exactly over the
 * run-up to the failure.
 *
 * Nothing writes to this ring unless a session has been active.
 *
 * ADR-0045 -- WALL CLOCK.  The first version recorded the emulated FRAME each
 * command landed on and nothing else, and that made it blind to the failure
 * the user could see with his own eyes: during a save the core emulates the
 * game's flash write, trips self-modifying-code detection ~4096 times, and a
 * frame stretches to 38 ms against a 33.4 ms budget.  In frame-count that is
 * indistinguishable from a healthy 16 ms frame, so a transcript could read
 * "perfectly regular" while the console was, in real time, falling behind a
 * peer that does NOT slow down with it.  It did, and I reported it as clean.
 *
 * So every entry now also carries a microsecond timestamp.  The clock is read
 * ONCE PER EMULATED FRAME, not once per command: the game issues ~600 commands
 * a frame, and ADR-0021 already established that a clock syscall on that path
 * costs milliseconds per frame.  Each command copies the cached value, which
 * is a load.  Per-frame resolution is all this needs -- the question is which
 * frames took 38 ms, not what happened inside one. */
#define RFU_FLIGHT_N 512
static u32 rfu_flight[RFU_FLIGHT_N * 2];   /* interleaved: word0, then us */
static u32 rfu_flight_head;      /* total entries ever written */
static u32 rfu_frame_no;
static u32 rfu_frame_us;         /* wall clock at the start of this frame */

/* Frontend supplies the clock; libretro builds get 0 and simply see no times.
 * Same weak-hook convention as the rest of the core/frontend seam here. */
u32 gpsp_rfu_now_us(void);
__attribute__((weak)) u32 gpsp_rfu_now_us(void) { return 0; }

/* Deepest either RFU receive queue has been, reported only when it grows
 * (ADR-0044).  `side` 0 = client receiving from host, 1 = host receiving from
 * a client.  This is the number that says whether RFU_PKT_QUEUE is big enough
 * for the bursts a stalled reliable transport delivers -- the previous depth
 * was chosen by argument twice and overflowed in the field both times. */
static u16 rfu_q_hi[2];

static void rfu_q_note(unsigned side, unsigned depth)
{
  if (depth > rfu_q_hi[side]) {
    rfu_q_hi[side] = (u16)depth;
    gpsp_rfu_trace_hook(RFU_TR_QHI, side, depth);
  }
}

/* cmd:8 | state:2 | frame:22, plus the frame's wall clock — two stores and a
 * load, no clock read, no branches beyond the mask. */
#define RFU_FLIGHT_REC(cmd, st) do { \
    u32 sl_ = (rfu_flight_head % RFU_FLIGHT_N) * 2; \
    rfu_flight[sl_] = \
        (((u32)(cmd) & 0xFF) << 24) | (((u32)(st) & 0x3) << 22) | \
        (rfu_frame_no & 0x3FFFFF); \
    rfu_flight[sl_ + 1] = rfu_frame_us; \
    rfu_flight_head++; \
  } while (0)

/* Pseudo-command codes for non-command entries.  Real RFU commands are all
 * below 0x40, so 0xFx cannot collide with one. */
#define RFU_FL_STATE   0xF0      /* low nibble of cmd carries the cause */

/* The ring is handed over in place -- entry i is at ((start+i) % cap) * 2 --
 * rather than copied into a local.  Copying 512 two-word entries would put 4 KB
 * on the emulation thread's stack at the one moment we least want a surprise. */
void gpsp_rfu_flight_dump_hook(const u32 *ring, unsigned cap, unsigned start,
                               unsigned n);
__attribute__((weak)) void gpsp_rfu_flight_dump_hook(const u32 *ring,
                                                     unsigned cap,
                                                     unsigned start, unsigned n)
{ (void)ring; (void)cap; (void)start; (void)n; }

static void rfu_flight_dump(void)
{
  unsigned n = rfu_flight_head < RFU_FLIGHT_N ? rfu_flight_head : RFU_FLIGHT_N;
  gpsp_rfu_flight_dump_hook(rfu_flight, RFU_FLIGHT_N, rfu_flight_head - n, n);
}

/* ADR-0052: the flight recorder dumps on ANY transition that ends a live
 * session, not only on RESET.  The client's graceful path is
 * CLIENT -> idle/DISC_PEER -> idle/RESET, so by the time RESET fires `was_`
 * is already IDLE -- the recorder stayed silent on precisely the console and
 * precisely the transition under investigation, and only the fatal path was
 * ever captured.  DISC_LOCAL/DISC_PEER from a live state end a session too,
 * and the ordering between the two consoles at that instant is the open
 * question: the client initiates the exit, yet the HOST is observed issuing
 * RFU_CMD_DISCONNECT (0x30) first while the client is still in CLIENT state.
 *
 * NOTE the comment lives OUT here: inside a `do {} while (0)` macro every
 * line needs a trailing backslash, comments included, or the definition ends
 * at the first line without one.  That mistake produced errors 200 lines
 * further down the file, nowhere near the cause. */
/* Set rfu_state and report the transition with its cause.  Three of the
 * nine transitions were previously silent, including the two that leave a
 * console's adapter in a state its game does not expect. */
#define RFU_SET_STATE(ns, cause) do { \
    int was_ = rfu_state; \
    rfu_state = (ns); \
    RFU_FLIGHT_REC(RFU_FL_STATE | ((cause) & 0x0F), (ns)); \
    gpsp_rfu_trace_hook(RFU_TR_STATE, (ns), (cause)); \
    if (((cause) == RFU_TRC_RESET || (cause) == RFU_TRC_DISC_PEER || \
         (cause) == RFU_TRC_DISC_LOCAL) && \
        (was_ == RFU_STATE_CLIENT || was_ == RFU_STATE_HOST)) \
      rfu_flight_dump(); \
  } while (0)

#define RFU_CONN_INPROGRESS  0x01000000     // Connection ongoing
#define RFU_CONN_FAILED      0x02000000     // Connection failed

#define RFU_CONN_COMP_FAIL   0x01000000     // Failed to connect

#define RFU_CMD_INIT1        0x10   // Dummy after-init command
#define RFU_CMD_INIT2        0x3d   // Dummy after-init command
#define RFU_CMD_SYSCFG       0x17   // System configuration (comms. config)

#define RFU_CMD_LINKPWR      0x11   // Link/RF strength (0 to 0x16/0xFF)

// These are not really well documented.
#define RFU_CMD_SYSVER       0x12   // Return some 1 word with version info.
#define RFU_CMD_SYSSTAT      0x13   // System/Connection status.
#define RFU_CMD_SLOTSTAT     0x14   // Reads slot status information.
#define RFU_CMD_CFGSTAT      0x15   // Reads some sort of config state.

#define RFU_CMD_BCST_DATA    0x16   // Broadcast data setup

#define RFU_CMD_HOST_START   0x19   // Start broadcasting and accepting clients
#define RFU_CMD_HOST_ACCEPT  0x1a   // Poll for incoming connections
#define RFU_CMD_HOST_STOP    0x1b   // Stop accepting new connections

#define RFU_CMD_BCRD_START   0x1c   // Broadcast read session start
#define RFU_CMD_BCRD_FETCH   0x1d   // Broadcast read (actual data read)
#define RFU_CMD_BCRD_STOP    0x1e   // Broadcast read session end

#define RFU_CMD_CONNECT      0x1f   // Connect to host
#define RFU_CMD_ISCONNECTED  0x20   // Check for conection status
#define RFU_CMD_CONCOMPL     0x21   // Complete connection?

#define RFU_CMD_SEND_DATA    0x24   // Sends a data packet
#define RFU_CMD_SEND_DATAW   0x25   // Sends a data packet and waits
#define RFU_CMD_RECV_DATA    0x26   // Receive (poll) some data
#define RFU_CMD_WAIT         0x27   // Wait (for response or timeout)
#define RFU_CMD_RTX_WAIT     0x37   // Wait after some retransmit?

#define RFU_CMD_DISCONNECT   0x30   // Disconnects clients

// RFU commands for slave mode (~command responses)
#define RFU_CMD_RESP_TIMEO   0x27   // Timeout!
#define RFU_CMD_RESP_DATA    0x28   // There's data available
#define RFU_CMD_RESP_DISC    0x29   // Some clients disconnected


// These are internal FSM states for the communication steps.
#define RFU_COMSTATE_RESET       0    // Just out of reset
#define RFU_COMSTATE_HANDSHAKE   1    // Performing initial nintendo handshake
#define RFU_COMSTATE_WAITCMD     2    // Idle, waiting for a command
#define RFU_COMSTATE_WAITDAT     3    // Waiting for follow up data
#define RFU_COMSTATE_RESPCMD     4    // RFU to device response (cmd)
#define RFU_COMSTATE_RESPDAT     5    // RFU to device response (N words)
#define RFU_COMSTATE_RESPERR     6    // Send back the error command
#define RFU_COMSTATE_RESPERR2    7    // Send back the error code
#define RFU_COMSTATE_WAITEVENT   8    // Waiting for event or timeout
#define RFU_COMSTATE_WAITRESP    9    // Responding a wait command

// These FSM states describe the adapter wifi state.
#define RFU_STATE_IDLE            0
#define RFU_STATE_HOST            1    // Hosting (with broadcast)
#define RFU_STATE_CONNECTING      2    // Trying to connect to a parent
#define RFU_STATE_CLIENT          3    // Client, connected to a host


static u32 rfu_prev_data;
static u32 rfu_comstate, rfu_cnt, rfu_state;
static u32 rfu_buf[255];
static u8 rfu_cmd, rfu_plen;
static u32 rfu_timeout_cycles, rfu_resp_timeout;
static u8 rfu_timeout, rfu_rtx_max;
/* SPI words spent in the current adapter-login phase (RFU_TR_LOGIN). */
static u32 rfu_login_words;
/* RFU_CMD_RECV_DATA commands served in this emulated frame, and the highest
 * such count seen so far (RFU_TR_RXBURST). */
static u32 rfu_rx_this_frame, rfu_rx_frame_hi;
/* ADR-0072: gate-trip rate and run length; see RFU_TR_RXGATE. */
static u32 rfu_gate_win, rfu_gate_n, rfu_gate_run, rfu_gate_run_max;
/* ADR-0072: our model of the GAME's recvQueue depth -- enqueue on delivery,
 * dequeue once per frame.  This, not a per-frame delivery count, is what
 * KeyInterCB_SelfIdle tests. */
static u32 rfu_gq_depth, rfu_gq_peak;
/* ADR-0072: inter-arrival clumping census; see RFU_TR_ARRIVAL. */
static u32 rfu_arr_last_frame, rfu_arr_clumped, rfu_arr_total;

/* ADR-0074: A CONTROL MESSAGE MUST NOT OVERTAKE THE DATA IT FOLLOWS.
 *
 * Leaving the Union Room is a NEGOTIATED teardown: one player picks the exit,
 * both games exchange dialogue ("exiting will end the session" / "please
 * wait"), both walk out, both get their card punched.  All of that travels as
 * ordinary link data, so on the client it sits in rfu_client.pkts[] waiting to
 * be handed to the game one packet per frame.
 *
 * The disconnect, however, was applied the instant it arrived -- and its
 * handler does `memset(&rfu_client, 0, ...)`, which is the same struct that
 * holds that queue.  So the messages that would have let the client exit
 * gracefully were deleted before the game ever saw them, and the game was
 * handed "all four slots disconnected" instead: the recoverable
 * "press A to return to lobby" screen, on a perfectly healthy link.
 *
 * The damage scales with queue depth, which is exactly the quantity the rate
 * mismatch inflates (ADR-0072/0073) -- an empty queue loses nothing, a backed
 * up one loses the whole handshake.  That is why it is the CLIENT that shows
 * this and effectively never the host.
 *
 * So: arm the disconnect, keep serving the queue, and go idle once the game
 * has actually caught up.  Bounded, so a queue that never drains cannot wedge
 * the link -- on expiry we do exactly what the old code did immediately.
 * 0 = the historical behaviour. */
static u32 rfu_disc_defer, rfu_disc_pending, rfu_disc_wait;

void rfu_set_disc_defer(u32 n) { rfu_disc_defer = n; }

/* ADR-0076: THE EXIT IS A RACE, AND THE CLIENT LOSES IT BY BEING SLOW.
 *
 * ADR-0074 deferred the peer disconnect until the client's queue drained, on
 * the theory that the teardown was destroying undelivered negotiation data.
 * The instrument said queued=1 -- premise dead, shipped disabled.  The decomp
 * says what the missing condition actually is, and it is TIME, not data:
 *
 * The negotiated exit on the child is a three-step sequence
 * (link_rfu_2.c:1132-1147, then :945-961):
 *   1. RFUCMD_DISCONNECT arrives as ordinary link DATA -> disconnectMode set,
 *      gReceivedRemoteLinkPlayers = 0, and a CLOCK-MASTER CHANGE is requested;
 *   2. the clock change completes over the next exchange(s), clearing
 *      lman.childClockSlave_flag;
 *   3. only then does RfuMain1_Child issue the child's OWN rfu_REQ_disconnect
 *      -- and with the Union Room having already set RFU_STATUS_LEAVE_GROUP,
 *      that path raises NO error.  This is the clean exit.
 *
 * Meanwhile the parent's librfu REQ_disconnects its slots, which reaches us
 * as NET_RFU_DISCONNECT on the wire.  If we go IDLE before step 3, the next
 * WAITEVENT answers RESP_DISC slots=0xF reason=DISCONNECTED, librfu's
 * linkWatcher raises LMAN_MSG_LINK_LOSS_DETECTED_AND_DISCONNECTED, and
 * pokeemerald's handler sets RFU_STATUS_CONNECTION_ERROR UNCONDITIONALLY
 * (link_rfu_2.c:2492) -- the "press A to return to lobby" screen, on a
 * perfectly healthy exit.  10 of 88 join logs and 0 of 88 host logs, because
 * only the child has steps 1-3 to finish and ours runs behind its host.
 *
 * So: hold the adapter in CLIENT state for a bounded grace window after the
 * peer's disconnect.  Queued data keeps being served; if the game issues its
 * own RFU_CMD_DISCONNECT inside the window it takes the clean local path and
 * the pending teardown is cancelled; on expiry we do exactly what the old
 * code did immediately.  Frames, 0 = off = historical behaviour.  60 frames
 * (~1 s) covers the clock change with an order of magnitude to spare while
 * keeping a genuinely dead link's error path intact. */
static u32 rfu_disc_grace, rfu_disc_grace_armed;

void rfu_set_disc_grace(u32 frames) { rfu_disc_grace = frames; }

static u32 rfu_client_queued(void);   /* defined after rfu_client */

/* Per-frame delivery cap for a CLIENT adapter, and the number of non-empty
 * reads already handed to the game this frame.  See ADR-0042.
 *
 * Field measurement (2026-08-03, two sessions, healthy transport): the joining
 * console is served up to SIX RFU_CMD_RECV_DATA in one emulated frame, where
 * the PPSSPP rig peaks at two or three and never reproduces the crash.  The
 * game drains its own 32-slot recvQueue once per frame and `full` is a LATCH
 * straight to RFU_STATUS_FATAL_ERROR -- the unrecoverable "turn the power off"
 * screen, which is exactly what the field sees and the rig does not.
 *
 * Holding a packet back costs it one frame of latency and nothing else: our
 * queue is RFU_PKT_QUEUE deep, which ADR-0011 widened 4 -> 16 precisely so
 * bunched arrivals would stop being dropped.  We report "no data this poll",
 * which is a reading the game already handles on every idle frame.
 *
 * 0 = unlimited = the historical behaviour, and remains the default.  Nothing
 * changes for anyone who does not set it. */
static u32 rfu_rx_cap;
static u32 rfu_rx_delivered;

/* ADR-0075: FRAME-BOUNDARY DELIVERY ADMISSION -- the fix the clumping data
 * asks for, built the way the cap post-mortem says it must be built.
 *
 * The measured facts this rests on (fs-gate, ~100 runs, both consoles):
 *   - `rfu_arrival` shows 5-45 % of host packets landing in the SAME emulated
 *     frame as their predecessor.  The game drains its recvQueue exactly once
 *     per frame, so every same-frame pair is one unit of net queue growth.
 *   - Worse, each delivery completes one data exchange, i.e. one MSC callback
 *     in the game's librfu.  TWO MSC events between two main-loop passes is
 *     exactly the send-queue ratchet condition (INPUT-FINDINGS): the child's
 *     sendQueue gains a level it never gives back, and at sendQueue >= 2
 *     pokeemerald's Gate A (`UpdateHeldKeyCode`, overworld.c:2453) silently
 *     nulls every DPAD/START/A keypress.  That is the input-eating bug.
 *   - The heldKeys probe (v7 fixture) confirmed on hardware that the game
 *     ingests the correct mask and the avatar still does not move: the
 *     discard is downstream, in the game, driven by queue depth.
 *
 * So: allow at most `rfu_frame_pace` non-empty deliveries to the game per
 * emulated frame.  One per frame equals both the game's drain rate and the
 * real adapter's radio cadence (one exchange per frame slot), so the game's
 * queues stay at their hardware-shaped depth BY CONSTRUCTION and neither
 * gate can trip.  A clump is spread over the frames that follow it; steady
 * one-per-frame traffic (mean is ~0.35/frame) is delivered with ZERO added
 * latency because the budget is spent on arrival, not held to the boundary.
 *
 * WHY THIS IS NOT THE FALSIFIED rfu_rx_cap: the cap withheld packets in
 * RECV_DATA while rfu_data_avail() kept announcing them.  In WAITEVENT the
 * adapter therefore answered RESP_DATA ("data available!"), the game polled,
 * was told "nothing", re-armed the wait, got RESP_DATA again -- the re-poll
 * spin that produced 610k trace events, burned the frame budget, collapsed
 * the transport (txq_hi 465, spill 81) and corrupted the very counters used
 * to judge the arm.  The pace gate is applied IDENTICALLY in BOTH places, so
 * an over-budget packet is simply invisible until the next frame: the wait
 * stays a wait, no poll is ever answered inconsistently, no spin exists.
 *
 * 0 = off = byte-for-byte the historical behaviour.  Harness key
 * `rfu_frame_pace`; hardware-validate before defaulting it on anywhere. */
static u32 rfu_frame_pace;
/* Set when the gate deferred an eligible packet this frame; consumed by the
 * once-per-window census in rfu_frame_update(). */
static u32 rfu_pace_held, rfu_pace_win, rfu_pace_n, rfu_pace_q_hi;

void rfu_set_frame_pace(u32 n)
{
  rfu_frame_pace = n;
}

/* CUSHION (fixed-depth jitter buffer, proof-of-concept).  Hold each host
 * packet for `rfu_cushion` emulated frames after it arrives before the game
 * may consume it.  In steady state this keeps ~rfu_cushion frames of host data
 * buffered ahead, so a network stall must outlast the whole reserve before the
 * client underruns and its WAITEVENT starves toward RESP_TIMEO -- the stall
 * half that frame_pace (the clump half) does not address.  MUST stay well under
 * RFU_DEF_TIMEOUT (32) so the deliberate hold never itself trips the timeout.
 * Gated IDENTICALLY in RECV_DATA and rfu_data_avail (the ADR-0075 discipline).
 * 0 = off = historical behaviour. */
static u32 rfu_cushion;
void rfu_set_cushion(u32 n)
{
  rfu_cushion = n;
}
/* Withheld polls this frame, and the highest such count seen (RFU_TR_RXHOLD).
 * Same reporting discipline as RFU_TR_RXBURST: a new high-water only. */
static u32 rfu_rx_held_this_frame, rfu_rx_held_hi;
/* ADR-0055: how long a host packet WAITED in our queue before the game
 * consumed it -- the client's answer latency.  The rfu_rx_cap experiment
 * showed the RFU protocol punishes a late reply far harder than it punishes
 * volume: delaying answers by a couple of frames collapsed the transport
 * (txq_hi 465, spill 81, the project's first net_error).  So the quantity
 * worth measuring is not how busy a frame was but how long the peer waited. */
static u32 rfu_ans_max_us, rfu_ans_sum_us, rfu_ans_n, rfu_ans_reported;

/* ADR-0060: CLIENT_ACK is a KEEPALIVE, so drive it from SILENCE, not traffic.
 *
 * It used to be sent on every single NET_RFU_HOST_SEND -- one extra reliable
 * payload per host packet, which is what made the client send 1.92x the host's
 * traffic (measured, 15 runs) and put its own liveness chatter into the ARQ
 * window to be retransmitted.
 *
 * The host uses it for exactly one thing: rfu_host.clients[].clttl = 0, against
 * a >= 240 frame (4 s) disconnect threshold -- and NET_RFU_CLIENT_SEND resets
 * that same counter. So while a client is talking, the ACK is pure redundancy.
 *
 * It is NOT redundant in general, which is why this rate-limits instead of
 * deleting: a game whose client is passive (host streams, client answers
 * nothing) would otherwise fall off the roster after 4 s. Sending only after a
 * second of OUR OWN silence keeps that case alive with 4x margin while removing
 * ~98% of the packets in a chatty one. A keepalive driven by how much traffic
 * there is scales with the very thing it exists to survive.
 *
 * u32 microseconds wrap every ~71 min; a wrap costs at most one early or late
 * keepalive against a 4 s budget, which is not worth a wider counter. */
#define RFU_CLIENT_KEEPALIVE_US 1000000u
static u32 rfu_cli_last_tx_us;

/* ADR-0058: frames between answer-latency censuses.  600 matches the existing
 * core_phase / sess_cost window, so a census line sits alongside the stats it
 * needs to be read against instead of drifting between them. */
#define RFU_ANS_CENSUS_FRAMES 600
static u32 rfu_ans_census;

/* ADR-0059 counters/knobs.  See the RFU_TR_TIMEO block above for why these
 * exist and why they are reported at most once per emulated frame. */
static u32 rfu_timeo_n,  rfu_timeo_rep;    /* client RESP_TIMEO */
static u32 rfu_noresp_n, rfu_noresp_rep;   /* host  synthetic no-answer */

/* ADR-0059 COMPENSATION KNOBS, in percent, 100 = stock.
 *
 * These stretch the two emulated-cycle deadlines above.  Be clear about what
 * this is: it is NOT a fidelity fix.  A real adapter grants T/59.7275 seconds;
 * running the session at 59.73 fps we already grant exactly that, and running
 * at 29.97 we grant TWICE what real hardware would.  We get away with 29.97
 * because of that 2x, not in spite of it.
 *
 * What is unfaithful here is the RADIO: a real GBA RFU link delivers in a few
 * milliseconds and PSP ad-hoc delivers in ~55 ms one-way.  The game's timeout
 * was sized for the former.  So these knobs deliberately grant MORE than real
 * hardware to compensate for a link an order of magnitude slower than the one
 * the game was written against -- the same class of concession as any netplay
 * latency hiding.  Setting them back to 100 does not make the emulator more
 * correct; it makes it correct about the wrong radio.
 *
 * scale = session_fps / 29.97 reproduces the known-good 29.97 wall-clock
 * budget at any session rate: 100 at 29.97, 150 at 45.00, 200 at 59.73. */
static u32 rfu_to_scale_x100  = 100;
static u32 rfu_rtx_scale_x100 = 100;

/* ADR-0062d: minimum emulated cycles between radio polls while the adapter is
 * in WAITEVENT.  Measured: the JOIN polls 587x per emulated frame and the HOST
 * 0x, because only the client waits on its peer.  Once the WLAN stack is
 * actually scheduled those polls stop being free -- 855 us per frame on the
 * join, 5 % of a 59.73 fps budget, to ask 587 times about one packet.
 * 0 = poll on every call = the historical behaviour. */
static u32 rfu_poll_min_cycles, rfu_poll_acc;

void rfu_set_poll_min_cycles(u32 n)
{
  rfu_poll_min_cycles = n;
  rfu_poll_acc = 0;
}

/* ADR-0068: minimum emulated cycles between radio polls while the adapter is
 * NOT in WAITEVENT -- i.e. the HOST's cadence.
 *
 * The knob above cuts the client's polling down; this one is the mirror image,
 * and it exists because the two roles are asymmetric in a way that was only
 * ever measured on the client side.  The host sits in WAITEVENT 0.6 % of the
 * time, so it never reaches the branch below, so the ONLY place it reads the
 * network is the single once-per-frame fe_np_pump().  Sending is already
 * immediate (netdrv txq_push transmits inside the window, nothing is Nagled),
 * so the host answers the instant it decides to -- it just does not LOOK until
 * the frame boundary, which puts up to one whole host frame inside the wait
 * the client is measuring.
 *
 * That also re-reads two numbers we already had: pumpgap's 13-16 ms mean was
 * filed as "the pump is not starved" (true) without noticing that one frame IS
 * the host's receive granularity, and the desktop rig's srtt of 16752 us --
 * exactly one emulated frame -- is an ACK sitting unread in a socket, not link
 * latency.
 *
 * netdrv_poll_needed() in front of the pump is syscall-free (ADR-0021), so a
 * poll that finds nothing costs a few hundred cycles, not a clock read.
 * 0 = never = the historical behaviour. */
static u32 rfu_idle_poll_cycles, rfu_idle_poll_acc;

void rfu_set_idle_poll_cycles(u32 n)
{
  rfu_idle_poll_cycles = n;
  rfu_idle_poll_acc = 0;
}

void rfu_set_timeout_scale(u32 to_x100, u32 rtx_x100)
{
  if (to_x100  <   25) to_x100  =   25;
  if (to_x100  > 1600) to_x100  = 1600;
  if (rtx_x100 <   25) rtx_x100 =   25;
  if (rtx_x100 > 1600) rtx_x100 = 1600;
  rfu_to_scale_x100  = to_x100;
  rfu_rtx_scale_x100 = rtx_x100;
}

void rfu_set_rx_cap(u32 n)
{
  rfu_rx_cap = n;
}

static struct {
  u32 buf[23];
  u8 blen;
} rfu_tx_buf;

static struct {
  u16 devid;         // Device ID assigned to the host
  u8 tx_ttl;         // Internal counter for broadcast transmission
  u32 bdata[6];      // Data to broadcast other devices
  struct {
    u16 client_id;   // libretro client ID
    u16 devid;       // Host-assigned device ID
    u16 clttl;       // Client TTL (to check for disconnects)
    struct {
      u32 datalen;   // Byte count of data waiting to be polled.
      u8  data[16];  // Data received from client.
    } pkts[RFU_PKT_QUEUE];
  } clients[4];      // Connected clients IDs (zero means empty slot).
} rfu_host;

static struct {
  u16 devid;         // Device ID assigned to the client (by the host?)
  u16 clnum;         // Client number (0 to 3)
  u16 host_id;       // Client ID for the host device.
  // Store host recevied packets (up to 8!)
  struct {
    u16 hblen;       // Bytes received from the host.
    u32 t_us;        // ADR-0055: when this packet ENTERED the queue.
    u32 arr_frame;   // CUSHION: emulated frame this packet arrived (for aging).
    u8 hdata[128];   // Data received from the host (accumulated).
  } pkts[RFU_PKT_QUEUE];
} rfu_client;

typedef struct {
  u8 valid;
  u8 ttl;            // When it reaches zero, entry is invalidated
  u16 device_id;     // Random ID generated by the RFU for each new session
  u32 data[6];       // Broadcast data (game+user data)
} t_client_broadcast;

// The table is indexed by client_id
static t_client_broadcast rfu_peer_bcst[MAX_RFU_PEERS];

// Constants used for the network protocol.

#define NET_RFU_HEADER          0x52465531       // RFU1

#define NET_RFU_BROADCAST       0x00    // Host to everyone broadcast packet
#define NET_RFU_CONNECT_REQ     0x01    // Client connection request
#define NET_RFU_CONNECT_ACK     0x02    // Host connection response (accept)
#define NET_RFU_CONNECT_NACK    0x03    // Host connection response (reject)
#define NET_RFU_DISCONNECT      0x04    // Client/Host disconnect notice
#define NET_RFU_HOST_SEND       0x05    // Host to client data send
#define NET_RFU_CLIENT_SEND     0x06    // Client to host data send
#define NET_RFU_CLIENT_ACK      0x07    // Client ACKs host received data.

// Callbacks used to send and force-receive data.
void netpacket_send(uint16_t client_id, const void *buf, size_t len);
void netpacket_send_unreliable(uint16_t client_id, const void *buf, size_t len);
void netpacket_poll_receive();

static void rfu_net_send_cmd(int client_id, u32 ptype, u32 h) {
  u32 pkt[4] = {
    netorder32(NET_RFU_HEADER),  // RFU1 header
    netorder32(ptype),           // Message type
    netorder32(h),               // Header word
    0
  };
  if (rfu_state == RFU_STATE_CLIENT)
    rfu_cli_last_tx_us = rfu_frame_us;   /* ADR-0060 keepalive timebase */
  netpacket_send(client_id, pkt, 16);
}

/* ADR-0060: liveness-only variant. See netpacket_send_unreliable(). */
static void rfu_net_send_cmd_unrel(int client_id, u32 ptype, u32 h) {
  u32 pkt[4] = {
    netorder32(NET_RFU_HEADER),
    netorder32(ptype),
    netorder32(h),
    0
  };
  if (rfu_state == RFU_STATE_CLIENT)
    rfu_cli_last_tx_us = rfu_frame_us;   /* ADR-0060 keepalive timebase */
  netpacket_send_unreliable(client_id, pkt, 16);
}

static void rfu_net_send_bcast(u32 ptype, u32 h, const u32 *pload) {
  u32 i;
  u32 pkt[9] = {
    netorder32(NET_RFU_HEADER),   // RFU1 header
    netorder32(ptype),            // Message type
    netorder32(h),                // Header word
  };
  for (i = 0; i < 6; i++)
    pkt[i + 3] = netorder32(pload[i]);

  // Broadcast to all clients (ID=0xffff)
  netpacket_send(0xffff, pkt, sizeof(pkt));
}

static void rfu_net_send_data(int client_id, u32 ptype, u32 h, const u32 *pload, unsigned plen) {
  u32 i;
  struct {
    u32 header[3];
    u8  data8[92];
  } pkt = {
    {
      netorder32(NET_RFU_HEADER),   // RFU1 header
      netorder32(ptype),            // Message type
      netorder32(h),                // Header word
    },
  };
  // Data is sent in little endian format over the RF link (presumably)
  for (i = 0; i < plen; i++)
    pkt.data8[i] = pload[i / 4] >> (8 * (i & 3));
  memset(&pkt.data8[plen], 0, 92 - plen);

  if (rfu_state == RFU_STATE_CLIENT)
    rfu_cli_last_tx_us = rfu_frame_us;   /* ADR-0060 keepalive timebase */
  netpacket_send(client_id, &pkt, 104);
}

// This is called whenever the game uses the GPIO (pin D) to perform a reset
// pin flip in the external reset pin. We reset the device to a known state.
void rfu_reset() {
  RFU_DEBUG_LOG("RFU reset!\n");
  // Reset FSMs to a known state
  rfu_prev_data = 0;
  rfu_cnt = 0;
  RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_RESET);
  /* Report how far the PREVIOUS login got before this reset threw it away:
   * a reset landing mid-handshake is what an AgbRFU_checkID failure looks
   * like from this side. */
  gpsp_rfu_trace_hook(RFU_TR_LOGIN, 0, rfu_login_words);
  rfu_login_words = 0;
  rfu_comstate = RFU_COMSTATE_RESET;
  rfu_timeout_cycles = 0;
  rfu_resp_timeout = 0;
  rfu_timeout = RFU_DEF_TIMEOUT;
  rfu_rtx_max = RFU_DEF_RTXMAX;
  /* ADR-0076: a reset ends any deferred teardown -- there is no session left
   * to tear down, and a stale pending would fire a spurious DISC_PEER after
   * the next session starts.  (Latent in ADR-0074 too; harmless there only
   * because the deferral shipped disabled.) */
  rfu_disc_pending     = 0;
  rfu_disc_grace_armed = 0;
  memset(&rfu_host, 0, sizeof(rfu_host));

  // Clear all the received broadcasts.
  memset(&rfu_peer_bcst, 0, sizeof(rfu_peer_bcst));

  // Re-seed random gen.  cpu_ticks is determined by the GBA program and
  // is therefore replay-reproducible; time(NULL) is not, and would break
  // record/replay or netplay rollback if used here.  The seed only needs
  // to vary across resets within a session, which cpu_ticks already
  // satisfies (rfu_reset is called from a GPIO pulse driven by the game).
  rand_seed(cpu_ticks);
}

static u16 new_devid() {
  while (1) {
    /* Mix in cpu_ticks (deterministic) rather than time(NULL) so the
     * generated device ID is reproducible across record/replay. */
    u16 n = rand_gen() ^ (u16)cpu_ticks;
    if (n)
      return n;
  }
}

// We have received a command in full (with a potential payload), process it
// and return the return command code (plus some payload too?).
static s32 rfu_process_command_inner(void);

/* Trace wrapper.  Reports the structural commands (the ones a session issues
 * a handful of times, not every frame) and — unconditionally — every ERROR
 * answer, which is what turns into the game's unrecoverable link screen. */
static s32 rfu_process_command(void) {
  s32 ret;
  /* Flight recorder gets EVERY command, unfiltered — the filter below is only
   * about what is worth a live log line.  The whole point of the ring is the
   * high-frequency traffic the live trace deliberately throws away. */
  RFU_FLIGHT_REC(rfu_cmd, rfu_state);
  switch (rfu_cmd) {
  /* Filtered: issued every frame while a link is up, would drown the log. */
  case RFU_CMD_LINKPWR: case RFU_CMD_SYSVER: case RFU_CMD_SYSSTAT:
  case RFU_CMD_SLOTSTAT: case RFU_CMD_CFGSTAT: case RFU_CMD_BCST_DATA:
  case RFU_CMD_BCRD_FETCH: case RFU_CMD_HOST_ACCEPT: case RFU_CMD_ISCONNECTED:
  case RFU_CMD_SEND_DATA: case RFU_CMD_SEND_DATAW: case RFU_CMD_RECV_DATA:
  case RFU_CMD_WAIT: case RFU_CMD_RTX_WAIT:
    break;
  default:
    gpsp_rfu_trace_hook(RFU_TR_CMD, rfu_cmd, rfu_state);
    break;
  }
  ret = rfu_process_command_inner();
  if (ret < 0)
    gpsp_rfu_trace_hook(RFU_TR_CMDERR, rfu_cmd, rfu_state);
  return ret;
}

static s32 rfu_process_command_inner(void) {
  u32 i, j, cnt = 0;
  RFU_DEBUG_LOG("Processing command 0x%x (len %d)\n", rfu_cmd, rfu_plen);

  switch (rfu_cmd) {
  // These are not 100% supported, but they are OK as long as we ACK them.
  case RFU_CMD_INIT1:
  case RFU_CMD_INIT2:
    return 0;

  case RFU_CMD_SYSCFG:
    // Contains the slave timeout and retransmit count.
    rfu_timeout = rfu_buf[0];
    rfu_rtx_max = rfu_buf[0] >> 8;
    /* ADR-0059: this is the one write to the adapter's timeout budget in a
     * whole run, and until now nothing logged it -- every timing table in
     * CLIFF-FINDINGS is parameterised on a value we could not see. */
    gpsp_rfu_trace_hook(RFU_TR_SYSCFG, rfu_timeout & 0xFFF, rfu_rtx_max & 0xFFF);
    return 0;

  case RFU_CMD_SYSVER:
    rfu_buf[0] = 0x00830117;   // Likely some sort of firmware/hw version.
    return 1;

  case RFU_CMD_SYSSTAT:
    // Lower bits contain the DEVID, along with slot bits (if any) and some status code.
    if (rfu_state == RFU_STATE_HOST)
      rfu_buf[0] = (1 << 24) | rfu_host.devid;
    else if (rfu_state == RFU_STATE_CLIENT)
      rfu_buf[0] = (5 << 24) | ((1 << rfu_client.clnum) << 16) | rfu_client.devid;
    else
      rfu_buf[0] = 0;

    return 1;

  case RFU_CMD_SLOTSTAT:
    if (rfu_state == RFU_STATE_HOST) {
      // Just a list of connected devices it seems
      u32 cnt = 0;
      rfu_buf[cnt++] = 0;
      for (i = 0; i < 4; i++)
        if (rfu_host.clients[i].devid) {
          rfu_buf[0]++;
          rfu_buf[cnt++] = rfu_host.clients[i].devid | (i << 16);
        }
      return cnt;
    }
    return 0;

  case RFU_CMD_LINKPWR:
    // TODO: Return something better (ie. latency?)
    if (rfu_state == RFU_STATE_HOST)
      rfu_buf[0] = (rfu_host.clients[0].devid ? 0x000000ff : 0) |
                   (rfu_host.clients[1].devid ? 0x0000ff00 : 0) |
                   (rfu_host.clients[2].devid ? 0x00ff0000 : 0) |
                   (rfu_host.clients[3].devid ? 0xff000000 : 0);
    else if (rfu_state == RFU_STATE_CLIENT)
      rfu_buf[0] = 0xffffffff;
    else
      rfu_buf[0] = 0;
    return 1;

  // Process broadcast read sessions.
  // TODO return errors if outside of a session.
  case RFU_CMD_BCRD_START:
    // ADR-0070: THIS is the moment the player asked for wireless, not the
    // login handshake.  See the comment on gpsp_rfu_activated_hook() below.
    gpsp_rfu_activated_hook();
    return 0;   // Return an ACK immediately.

  case RFU_CMD_BCRD_STOP:
  case RFU_CMD_BCRD_FETCH:
    // We pick up to four random broadcasting peers.
    // This is not randomly fair but whatever :D
    i = rand_gen() % MAX_RFU_PEERS;
    for (j = 0, cnt = 0; cnt < 4*7 && j < MAX_RFU_PEERS; j++) {
      u32 entry = (i + j) % MAX_RFU_PEERS;
      if (rfu_peer_bcst[entry].valid) {
        // Header is just the device ID
        rfu_buf[cnt++] = rfu_peer_bcst[entry].device_id;
        memcpy(&rfu_buf[cnt], rfu_peer_bcst[entry].data,
               sizeof(rfu_peer_bcst[entry].data));
        cnt += 6;
      }
    }
    return cnt;

  // Sets the broadcast data to use on Host mode
  case RFU_CMD_BCST_DATA:
    if (rfu_plen == 6)
      memcpy(rfu_host.bdata, rfu_buf, sizeof(rfu_host.bdata));
    return 0;

  case RFU_CMD_HOST_START:
    gpsp_rfu_activated_hook();   /* ADR-0070, see BCRD_START above */
    if (rfu_state == RFU_STATE_CLIENT) {
      RFU_DEBUG_LOG("RFU error: Cannot start host, we are a client already\n");
      return -1;   // Return error if we are connected (state client)
    }

    if (rfu_state == RFU_STATE_IDLE) {
      // Generate a new ID if we don't have one already.
      rfu_host.devid = new_devid();
      memset(rfu_host.clients, 0, sizeof(rfu_host.clients));
      RFU_SET_STATE(RFU_STATE_HOST, RFU_TRC_HOST_START);
      RFU_DEBUG_LOG("Start hosting with device ID %02x\n", rfu_host.devid);
    }
    // Start broadcasting immediately.
    rfu_host.tx_ttl = 0xff;
    return 0;

  case RFU_CMD_HOST_STOP:
    if (rfu_state == RFU_STATE_IDLE)
      return -1;  // Return error if host mode is not active

    // This just "stops" accepting new clients, however if the host has no
    // clients, it will return to idle state (I think!).
    if (rfu_state == RFU_STATE_HOST) {
      for (i = 0; i < 4; i++)
        if (rfu_host.clients[i].devid)
          return 0;     // We stay in the host mode.

      RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_HOST_STOP);
    }

    return 0;

  case RFU_CMD_HOST_ACCEPT:
    if (rfu_state == RFU_STATE_IDLE)
      return -1;  // Return error if host mode is not active

    // This is actually a "list connected devices", not actually accept().
    // Return a list of IDs and a client number (slot number).
    for (i = 0; i < 4; i++)
      if (rfu_host.clients[i].devid)
        rfu_buf[cnt++] = rfu_host.clients[i].devid | (i << 16);
    return cnt;

  case RFU_CMD_CONNECT:
    if (rfu_state == RFU_STATE_HOST)
      return -1;  // Return error if host mode is active

    // The game specified a device ID, find the host.
    {
      u16 reqid = rfu_buf[0] & 0xffff;
      for (i = 0, cnt = 0; i < MAX_RFU_PEERS; i++) {
        if (rfu_peer_bcst[i].valid &&
            rfu_peer_bcst[i].device_id == reqid) {

          // Send a request to the host to connect
          rfu_net_send_cmd(i, NET_RFU_CONNECT_REQ, reqid);
          RFU_SET_STATE(RFU_STATE_CONNECTING, RFU_TRC_CONNECT);
          RFU_DEBUG_LOG("Requesting connection to client %d (%x)\n", i, reqid);
          return 0;
        }
      }
    }
    // If the ID cannot be found, just ACK for now, ISCONNECTED will fail.
    return 0;

  case RFU_CMD_ISCONNECTED:
    if (rfu_state == RFU_STATE_HOST)
      return -1;  // Return error if host mode is active

    if (rfu_state == RFU_STATE_CONNECTING)
      rfu_buf[0] = RFU_CONN_INPROGRESS;
    else if (rfu_state == RFU_STATE_IDLE)
      rfu_buf[0] = RFU_CONN_FAILED;
    else
      rfu_buf[0] = rfu_client.devid | (rfu_client.clnum << 16);

    return 1;

  case RFU_CMD_CONCOMPL:
    // Seems that this is also called even when no connection happened!
    if (rfu_state == RFU_STATE_HOST)
      return -1;

    // Returns ID, with slot number and can also indicate failure.
    if (rfu_state == RFU_STATE_CLIENT) {
      rfu_buf[0] = rfu_client.devid | (rfu_client.clnum << 16);
    } else {
      rfu_buf[0] = RFU_CONN_COMP_FAIL;
      RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_CONCOMPL_FAIL);
    }

    return 1;

  case RFU_CMD_SEND_DATAW:
  case RFU_CMD_SEND_DATA:
    if (!rfu_plen)
      return 0;

    if (rfu_state == RFU_STATE_HOST) {
      // Read data to be sent into the TX buffer
      rfu_tx_buf.blen = rfu_buf[0] & 0x7F;
      memcpy(rfu_tx_buf.buf, &rfu_buf[1], (rfu_plen - 1)*sizeof(u32));
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      // Same as above, but the header encoding is funny
      rfu_tx_buf.blen = (rfu_buf[0] >> (8 + rfu_client.clnum * 5)) & 0x1F;
      memcpy(rfu_tx_buf.buf, &rfu_buf[1], (rfu_plen - 1)*sizeof(u32));
    }

    /* fallthrough */
  case RFU_CMD_RTX_WAIT:
    if (rfu_state == RFU_STATE_HOST) {
      // Host sends a package to all clients.
      RFU_DEBUG_LOG("Host sending %d bytes / %d words to clients\n",
                    rfu_tx_buf.blen, rfu_plen - 1);
      if (rfu_tx_buf.blen <= 90) {
        for (i = 0; i < 4; i++)
          if (rfu_host.clients[i].devid)
            rfu_net_send_data(rfu_host.clients[i].client_id,
              NET_RFU_HOST_SEND, rfu_tx_buf.blen, rfu_tx_buf.buf, rfu_tx_buf.blen);
      }
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      // Schedule data to be sent
      RFU_DEBUG_LOG("Client sending %d bytes / %d words to host\n",
                    rfu_tx_buf.blen, rfu_plen - 1);
      if (rfu_tx_buf.blen <= 16) {
        // Send it immediately! This is not really accurate.
        rfu_net_send_data(rfu_client.host_id, NET_RFU_CLIENT_SEND,
          (rfu_tx_buf.blen << 24) | (rfu_client.clnum << 16) | rfu_client.devid,
          rfu_tx_buf.buf, rfu_tx_buf.blen);
      }
    }
    else
      return -1;   // We are not connected nor a host
    break;

  case RFU_CMD_RECV_DATA:
    rfu_rx_this_frame++;
    if (rfu_state == RFU_STATE_HOST) {
      // Receive data from clients
      u32 cnt = 0, bufbytes = 0;
      u8 tmpbuf[16*4] = {0};
      rfu_buf[cnt++] = 0;   // Header contains byte counts as a bitfield.
      for (i = 0; i < 4; i++) {
        u32 dlen = MIN(16, rfu_host.clients[i].pkts[0].datalen);
        if (rfu_host.clients[i].devid && dlen != 0) {
          RFU_DEBUG_LOG("Host reads data from client buffer (%d bytes)\n", dlen);
          // Accumulate into temp buffer
          memcpy(&tmpbuf[bufbytes], &rfu_host.clients[i].pkts[0].data[0], dlen);
          bufbytes += dlen;
          // Update byte count header for this client
          rfu_buf[0] |= dlen << (8 + i * 5);
          // Discard front packet
          memmove(&rfu_host.clients[i].pkts[0], &rfu_host.clients[i].pkts[1],
                  (RFU_PKT_QUEUE - 1) * sizeof(rfu_host.clients[i].pkts[0]));
          rfu_host.clients[i].pkts[RFU_PKT_QUEUE - 1].datalen = 0;
        }
      }
      // Copy data into words into the RFU buffer.
      for (i = 0; i < (bufbytes + 3) / 4; i++)
        rfu_buf[cnt++] = leupack32(&tmpbuf[i*4]);
      return cnt;
    }
    else if (rfu_state == RFU_STATE_CLIENT) {
      u32 cnt = 0;
      u32 dlen = rfu_client.pkts[0].hblen;

      /* ADR-0075: the pace gate, mirrored EXACTLY in rfu_data_avail() -- the
       * two must never disagree, or the WAITEVENT re-poll spin that sank the
       * cap experiment comes back.  See the block comment at rfu_frame_pace. */
      if (rfu_frame_pace && dlen != 0 && rfu_rx_delivered >= rfu_frame_pace) {
        rfu_pace_held = 1;
        rfu_buf[cnt++] = 0;
        return cnt;
      }

      /* Delivery pacing (ADR-0042).  Once this frame's quota of real packets
       * is spent, answer "nothing yet" and leave the packet queued -- do NOT
       * fall through to the memmove below, or holding back would discard. */
      if (rfu_rx_cap != 0 && dlen != 0 && rfu_rx_delivered >= rfu_rx_cap) {
        /* Counted here, reported at most once per frame from
         * rfu_frame_update().  Tracing every hold is not an option: the game
         * re-polls a withheld packet for the rest of the frame, so a cap of 1
         * produced 610k events in one session and the logging alone broke the
         * run it was supposed to measure. */
        rfu_rx_held_this_frame++;
        rfu_buf[cnt++] = 0;
        return cnt;
      }

      /* CUSHION: hold the oldest packet until it has aged rfu_cushion frames,
       * so the game runs on a buffered-ahead reserve.  Mirrored EXACTLY in
       * rfu_data_avail() -- an unaged packet must be invisible in BOTH or the
       * WAITEVENT re-poll spin returns (ADR-0075). */
      if (dlen != 0 && rfu_cushion &&
          (u32)(rfu_frame_no - rfu_client.pkts[0].arr_frame) < rfu_cushion) {
        rfu_buf[cnt++] = 0;
        return cnt;
      }

      RFU_DEBUG_LOG("Client reads data from host buffer (%d bytes)\n", dlen);
      rfu_buf[cnt++] = dlen;   // Header contains byte count.
      for (j = 0; j < (dlen + 3) / 4; j++)
        rfu_buf[cnt++] = leupack32(&rfu_client.pkts[0].hdata[j*4]);

      if (dlen != 0)
        rfu_rx_delivered++;

      // Move to the next packet
      /* ADR-0055: slot 0 is about to be handed to the game.  Its wait is the
       * client's answer latency for this packet.  Frame-granular (rfu_frame_us
       * is the once-per-frame clock), which is the right resolution: what
       * matters is whether a packet sat for MULTIPLE frames, not sub-frame
       * jitter. */
      if (dlen != 0 && rfu_client.pkts[0].t_us != 0 &&
          rfu_frame_us >= rfu_client.pkts[0].t_us) {
        u32 waited = rfu_frame_us - rfu_client.pkts[0].t_us;
        if (waited > rfu_ans_max_us)
          rfu_ans_max_us = waited;
        rfu_ans_sum_us += waited;
        rfu_ans_n++;
      }
      memmove(&rfu_client.pkts[0], &rfu_client.pkts[1],
              sizeof(rfu_client.pkts[0]) * (RFU_PKT_QUEUE - 1));
      rfu_client.pkts[RFU_PKT_QUEUE - 1].hblen = 0;
      return cnt;
    }
    break;

  case RFU_CMD_WAIT:
    // Do nothing, return no data (just ack).
    // The handler will deal with clock reversing.
    return 0;

  case RFU_CMD_DISCONNECT:
    if (rfu_state == RFU_STATE_CLIENT) {
      /* ADR-0076: the game beat the grace window -- THE RACE WAS WON.  Cancel
       * the pending peer teardown so the clean local exit below is the only
       * one that runs.  mode=5 in the trace = grace succeeded. */
      if (rfu_disc_pending && rfu_disc_grace_armed)
        gpsp_rfu_trace_hook(RFU_TR_DISCQ, rfu_client_queued(), 5);
      rfu_disc_pending     = 0;
      rfu_disc_grace_armed = 0;
      // Assuming self-disconnect?
      rfu_net_send_cmd(rfu_client.host_id, NET_RFU_DISCONNECT,
                       rfu_client.devid | (rfu_client.clnum << 16));
      RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_DISC_LOCAL);
      gpsp_rfu_link_down_hook(RFU_DOWN_LOCAL, rfu_client.clnum);
    } else if (rfu_state == RFU_STATE_HOST) {
      // We are disconnecing some client(s).
      for (i = 0; i < 4; i++)
        if (rfu_buf[0] & (1 << i)) {
          // Send disconnect notice, clear slot!
          rfu_net_send_cmd(rfu_host.clients[i].client_id, NET_RFU_DISCONNECT,
                   rfu_host.clients[i].devid | (i << 16));
          memset(&rfu_host.clients[i], 0, sizeof(rfu_host.clients[i]));
          gpsp_rfu_link_down_hook(RFU_DOWN_LOCAL, i);
        }
    }
    return 0;

  default:
    RFU_DEBUG_LOG("Unknown RFU command %02x\n", rfu_cmd);
    /* Answered with a bare ACK and no data.  Some of these are real
     * commands this adapter model never implemented (0x32/0x33/0x34 =
     * librfu's CPR_START/POLL/END link-recovery trio, 0x35/0x36), and a
     * bare ACK where the game expects a status word is not obviously
     * benign — so make them visible instead of silent. */
    gpsp_rfu_trace_hook(RFU_TR_UNKCMD, rfu_cmd, rfu_state);
  };

  return 0;
}

// Returns true if a Wait event can finish due to new data being available.
/* Undelivered host packets sitting in the client's queue. */
static u32 rfu_client_queued(void)
{
  u32 i, n = 0;
  for (i = 0; i < RFU_PKT_QUEUE; i++)
    if (rfu_client.pkts[i].hblen)
      n++;
  return n;
}

static bool rfu_data_avail() {
  if (rfu_state == RFU_STATE_CLIENT) {
    if (rfu_client.pkts[0].hblen != 0) {
      /* ADR-0075: an over-budget packet must be invisible HERE too, or
       * WAITEVENT answers RESP_DATA for a packet RECV_DATA then refuses --
       * the exact inconsistency behind the cap experiment's re-poll spin. */
      if (rfu_frame_pace && rfu_rx_delivered >= rfu_frame_pace) {
        rfu_pace_held = 1;
        return false;
      }
      /* CUSHION mirror (see RECV_DATA): a not-yet-aged packet is invisible so
       * the wait stays a wait rather than answering RESP_DATA inconsistently. */
      if (rfu_cushion &&
          (u32)(rfu_frame_no - rfu_client.pkts[0].arr_frame) < rfu_cushion)
        return false;
      return true;
    }
  }
  else if (rfu_state == RFU_STATE_HOST) {
    // Returns true if any data from any client is available.
    u32 i;
    for (i = 0; i < 4; i++)
      if (rfu_host.clients[i].devid && rfu_host.clients[i].pkts[0].datalen)
        return true;
  }
  return false;
}

// Called whenever the game flips the SIOCNT:Start bit to transmit a value.
// We simulate the reception and response of said value (over the SPI bus).
u32 rfu_transfer(u32 sent_value) {
  u32 retval = 0x80000000;

  switch (rfu_comstate) {
  case RFU_COMSTATE_RESET:
    // Start of sequence (check the low 16 bits)
    retval = 0;
    rfu_login_words++;
    if ((sent_value & 0xFFFF) == 0x494E) {
      rfu_comstate = RFU_COMSTATE_HANDSHAKE;
      gpsp_rfu_trace_hook(RFU_TR_LOGIN, 1, rfu_login_words);
      rfu_login_words = 0;
    }

    break;

  case RFU_COMSTATE_HANDSHAKE:
    // Check for the last step of the sequence
    rfu_login_words++;
    if (sent_value == 0xB0BB8001) {
      rfu_comstate = RFU_COMSTATE_WAITCMD;
      gpsp_rfu_trace_hook(RFU_TR_LOGIN, 2, rfu_login_words);
      rfu_login_words = 0;
      /* ADR-0070: THE ACTIVATION HOOK USED TO FIRE HERE, AND IT WAS WRONG.
       *
       * ADR-0013 assumed adapter login was "the exact moment the player used
       * an in-game wireless feature".  Measured on hardware, it is not:
       * Emerald probes the adapter during BOOT -- the first `logged_in` lands
       * at emulated frame 3, between the frontend's f=1 and f=9 input events
       * -- and then logs in TEN MORE TIMES per session as the game resets and
       * re-initialises the device.  Login means "an adapter is present",
       * which is how the game decides to show its wireless title screen.  It
       * says nothing about intent.
       *
       * Firing here made the silent-wireless variants bring the radio up at
       * boot, which breaks `role = auto` outright: the join-first window is
       * 240 + 0..119 frames, so two consoles started even ten seconds apart
       * never overlap, both promote to host, and neither ever sees the other.
       * The jittered backoff cannot help -- it is sized for simultaneous
       * activation, which boot is not.
       *
       * The hook now fires on BCRD_START and HOST_START instead: the commands
       * a game issues when it actually begins looking for a peer.  Measured
       * in the same run, BCRD_START lands ~400 log lines in, immediately
       * before CONNECT and CONCOMPL and long before the trade seat -- i.e. at
       * the attendant, where both players are within seconds of each other.
       *
       * The hook is idempotent by contract; the frontend latches it. */
    }

    retval = (sent_value << 16) | ((~rfu_prev_data) & 0xFFFF);
    break;

  case RFU_COMSTATE_WAITCMD:
    // Wait for a new command, verify its header.
    if ((sent_value >> 16) == 0x9966) {
      rfu_plen = (u8)(sent_value >> 8);
      rfu_cmd  = (u8)(sent_value);
      rfu_cnt = 0;
      if (!rfu_plen) {
        // Returns error code or response length
        s32 ret = rfu_process_command();
        if (ret < 0) {
          rfu_comstate = RFU_COMSTATE_RESPERR;
          rfu_cmd = (u32)(-ret); // Err code
          rfu_plen = 1;
        }
        else {
          rfu_comstate = RFU_COMSTATE_RESPCMD;
          rfu_plen = (u32)ret;
        }
      }
      else
        rfu_comstate = RFU_COMSTATE_WAITDAT;
    }
    break;

  case RFU_COMSTATE_WAITDAT:
    rfu_buf[rfu_cnt++] = sent_value;
    if (rfu_cnt == rfu_plen) {
      s32 ret = rfu_process_command();
      if (ret < 0) {
        rfu_comstate = RFU_COMSTATE_RESPERR;
        rfu_cmd = (u32)(-ret); // Err code
        rfu_plen = 1;
      }
      else {
        rfu_comstate = RFU_COMSTATE_RESPCMD;
        rfu_plen = (u32)ret;
      }
      rfu_cnt = 0;
    }
    break;

  case RFU_COMSTATE_RESPCMD:
    // Disregard the input value and respond with the ack'ed command
    retval = 0x99660080 | rfu_cmd | (rfu_plen << 8);

    // These commands are special: they do not have any response data
    // and reverse the clock roles (the RFU becomes master).
    if (rfu_cmd == RFU_CMD_WAIT || rfu_cmd == RFU_CMD_RTX_WAIT || rfu_cmd == RFU_CMD_SEND_DATAW) {
      rfu_comstate = RFU_COMSTATE_WAITEVENT;
      /* ADR-0059: ... x scale/100.  The divisions are folded into the
       * constants to stay in u32: 16777216/60/100 = 2796 (0.007 % low against
       * the exact 2796.20), 16777216/60/6/100 = 466. */
      rfu_timeout_cycles = rfu_timeout * (16777216 / 60 / 100) * rfu_to_scale_x100;
      rfu_resp_timeout = rfu_rtx_max * (16777216 / 60 / 6 / 100) * rfu_rtx_scale_x100;
    } else {
      rfu_comstate = rfu_plen ? RFU_COMSTATE_RESPDAT : RFU_COMSTATE_WAITCMD;
    }
    break;

  case RFU_COMSTATE_RESPDAT:
    // Keep pushing data to the GBA
    retval = rfu_buf[rfu_cnt++];
    if (rfu_cnt == rfu_plen)
      rfu_comstate = RFU_COMSTATE_WAITCMD;
    break;

  case RFU_COMSTATE_WAITEVENT:
  case RFU_COMSTATE_WAITRESP:
    // TODO: this should not happen? Since we are master?
    break;

  case RFU_COMSTATE_RESPERR:
    // Return the error code command. Includes one error value
    retval = 0x996601ee;
    rfu_comstate = RFU_COMSTATE_RESPERR2;
    break;

  case RFU_COMSTATE_RESPERR2:
    retval = rfu_cmd;   // Some error code, not understood yet.
    rfu_comstate = RFU_COMSTATE_WAITCMD;
    break;
  };

  rfu_prev_data = sent_value;
  return retval;
}

// Gets called every frame for basic device updates.
void rfu_frame_update() {
  /* One emulated frame's worth of RECV_DATA has just ended.  The game drains
   * its own 32-slot recvQueue once per frame, so this count IS the queue's
   * per-frame fill rate; report each new high-water (see RFU_TR_RXBURST). */
  /* ADR-0072: MODEL THE GAME'S QUEUE, DO NOT PROXY IT.
   *
   * The first version of this counted `rfu_rx_this_frame`, and that was wrong
   * twice over.  That counter is incremented at the TOP of RFU_CMD_RECV_DATA
   * (:1026), before the cap check and regardless of whether a packet is handed
   * over, so it counts the game's POLLS, not our deliveries -- and the client
   * polls hard precisely when it is waiting and receiving nothing.  It is also
   * the counter behind RFU_TR_RXBURST, so the "join bursts reach 9 against the
   * host's 1-2" reading is a poll-rate asymmetry, NOT evidence that we deliver
   * in clumps.  Everything inferred from that number has to be re-earned.
   *
   * What actually gates the player's input is the depth of the GAME's
   * recvQueue, and a per-frame delivery count cannot express it: one burst of
   * 9 holds the queue above the threshold for several frames afterwards, and a
   * spike counter records exactly one.
   *
   * So model it. Every delivered packet is one enqueue; the game dequeues
   * exactly once per frame (link_rfu_2.c:937 via link.c:1798). Two adds and a
   * clamp, and the result is the quantity pokeemerald's KeyInterCB_SelfIdle
   * actually tests.
   *
   * `rfu_rx_delivered` is still live here -- it is reset further down. */
  rfu_gq_depth += rfu_rx_delivered;
  if (rfu_gq_depth)
    rfu_gq_depth--;                       /* the game's once-per-frame drain */
  if (rfu_gq_depth > rfu_gq_peak)
    rfu_gq_peak = rfu_gq_depth;
  if (rfu_gq_depth > RFU_GAME_RECVQ_GATE) {
    rfu_gate_n++;
    if (++rfu_gate_run > rfu_gate_run_max)
      rfu_gate_run_max = rfu_gate_run;
  } else {
    rfu_gate_run = 0;
  }
  if (++rfu_gate_win >= RFU_RXGATE_FRAMES) {
    /* Peak in the high half, over-gate frame count in the low half: the count
     * cannot exceed the 600-frame window, so 16 bits is ample for both. */
    gpsp_rfu_trace_hook(RFU_TR_RXGATE, rfu_gate_n & 0xFFF, rfu_gate_run_max);
    /* ADR-0072 fix: peak on its own event so the ring's 12-bit field keeps it. */
    gpsp_rfu_trace_hook(RFU_TR_GQPEAK, rfu_gq_peak, rfu_gate_n & 0xFFF);
    rfu_gate_win = 0;
    rfu_gate_n = 0;
    rfu_gate_run_max = 0;
    rfu_gq_peak = 0;
  }

  /* ADR-0075: pace census.  Counted per frame, emitted per window, and only
   * while the knob is on -- when it is off there is nothing to distinguish
   * from the historical build and a permanent n=0 line would only add noise.
   * When it is ON, the line appears every window even at n=0, so "the gate
   * never bound" and "the instrument is dead" cannot be confused (ADR-0058). */
  if (rfu_pace_held) {
    u32 q = rfu_client_queued();
    rfu_pace_n++;
    if (q > rfu_pace_q_hi)
      rfu_pace_q_hi = q;
    rfu_pace_held = 0;
  }
  if (++rfu_pace_win >= RFU_RXGATE_FRAMES) {
    if (rfu_frame_pace)
      gpsp_rfu_trace_hook(RFU_TR_PACE, rfu_pace_n, rfu_pace_q_hi);
    rfu_pace_win = 0;
    rfu_pace_n = 0;
    rfu_pace_q_hi = 0;
  }

  /* ADR-0074: apply a deferred peer disconnect once the game has caught up.
   * Checked at frame end, after this frame's RECV_DATA polls have had their
   * chance to drain the queue. */
  if (rfu_disc_pending) {
    /* ADR-0076: a grace-armed deferral waits its FULL window -- the game may
     * still be mid clock-change with an empty queue, which is exactly the
     * moment ADR-0074's queue-empty condition would have torn down.  The
     * clean exit cancels the pending from RFU_CMD_DISCONNECT instead.
     * mode=4 in the trace = grace EXPIRED, historical teardown applied. */
    int expire = rfu_disc_grace_armed
        ? (rfu_disc_wait == 0)
        : (!rfu_client_queued() || rfu_disc_wait == 0);
    if (expire) {
      u32 clnum = rfu_client.clnum;
      gpsp_rfu_trace_hook(RFU_TR_DISCQ, rfu_client_queued(),
                          rfu_disc_grace_armed ? 4 : 2);
      memset(&rfu_client, 0, sizeof(rfu_client));
      RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_DISC_PEER);
      gpsp_rfu_link_down_hook(RFU_DOWN_PEER, clnum);
      rfu_disc_pending     = 0;
      rfu_disc_grace_armed = 0;
    } else {
      rfu_disc_wait--;
    }
  }

  if (rfu_rx_this_frame > rfu_rx_frame_hi) {
    gpsp_rfu_trace_hook(RFU_TR_RXBURST, rfu_rx_this_frame, rfu_rx_frame_hi);
    rfu_rx_frame_hi = rfu_rx_this_frame;
  }
  rfu_rx_this_frame = 0;
  rfu_frame_no++;          /* ADR-0043 flight-recorder timebase */
  rfu_frame_us = gpsp_rfu_now_us();   /* ADR-0045: ONE clock read per frame */

  /* ADR-0042: report the withheld-poll high-water, then refill the quota. */
  if (rfu_rx_held_this_frame > rfu_rx_held_hi) {
    gpsp_rfu_trace_hook(RFU_TR_RXHOLD, rfu_rx_held_this_frame, rfu_rx_delivered);
    rfu_rx_held_hi = rfu_rx_held_this_frame;
  }
  rfu_rx_held_this_frame = 0;
  rfu_rx_delivered       = 0;

  /* ADR-0055: report a new answer-latency high-water, with the running mean
   * alongside it so a single outlier cannot be mistaken for a trend. */
  if (rfu_ans_max_us > rfu_ans_reported) {
    gpsp_rfu_trace_hook(RFU_TR_ANSWER, rfu_ans_max_us,
                        rfu_ans_n ? rfu_ans_sum_us / rfu_ans_n : 0);
    rfu_ans_reported = rfu_ans_max_us;
  }

  /* ADR-0058: PERIODIC CENSUS -- report even when nothing is wrong.
   *
   * The high-water emit above is silent unless a NEW maximum appears, so a
   * session in which every packet is answered within one frame produces max=0,
   * `0 > 0` is false, and the probe never says a word.  That is byte-for-byte
   * the same output as a probe that was never wired up, which is exactly how
   * this looked across runs 6-9: no rfu_answer line at all, and no way to tell
   * "answer latency is sub-frame" from "the instrument is dead".  A measurement
   * you cannot distinguish from a broken measurement is not evidence.
   *
   * So emit the SAMPLE COUNT periodically. n>0 with max=0 means genuinely
   * sub-frame; n=0 means nothing was ever measured and the probe placement is
   * wrong. Counters are cumulative -- this is a census, not a window. */
  if (++rfu_ans_census >= RFU_ANS_CENSUS_FRAMES) {
    rfu_ans_census = 0;
    gpsp_rfu_trace_hook(RFU_TR_ANSSTAT, rfu_ans_n, rfu_ans_max_us);
  }

  /* ADR-0059: the adapter's own deadlines.  Report on CHANGE, once per frame
   * at most, so a timeout that fires many times in one frame cannot flood the
   * 64-entry trace ring and take the other measurements with it.  The counts
   * are cumulative, so consecutive lines give the per-frame rate directly. */
  if (rfu_timeo_n != rfu_timeo_rep) {
    rfu_timeo_rep = rfu_timeo_n;
    gpsp_rfu_trace_hook(RFU_TR_TIMEO, rfu_timeo_n & 0xFFF, rfu_timeout & 0xFFF);
  }
  if ((rfu_noresp_n >> 4) != rfu_noresp_rep) {
    rfu_noresp_rep = rfu_noresp_n >> 4;
    gpsp_rfu_trace_hook(RFU_TR_NORESP, rfu_noresp_rep & 0xFFF,
                        rfu_rtx_max & 0xFFF);
  }

  // If device is in reset state, do nothing really.
  if (rfu_comstate != RFU_COMSTATE_RESET) {
    u32 i;
    // Account for peer expiration.
    for (i = 0; i < MAX_RFU_PEERS; i++) {
      if (rfu_peer_bcst[i].valid) {
        if (--rfu_peer_bcst[i].ttl == 0)
          rfu_peer_bcst[i].valid = 0;
      }
    }

    // Broadcast host session periodically
    if (rfu_state == RFU_STATE_HOST) {
      if (rfu_host.tx_ttl++ >= BCST_ANNOUNCE_VB) {
        rfu_host.tx_ttl = 0;
        rfu_net_send_bcast(NET_RFU_BROADCAST, rfu_host.devid, rfu_host.bdata);
      }
    }

    // Account client TTL (to timeout clients)
    if (rfu_state == RFU_STATE_HOST) {
      for (i = 0; i < 4; i++) {
        if (rfu_host.clients[i].devid) {
          if (++rfu_host.clients[i].clttl >= 240 /* 4s */) {
            // The client hasn't sent stuff for a while, disconnect.
            RFU_DEBUG_LOG("Disconnect client slot %d due to timeout\n", i);
            memset(&rfu_host.clients[i], 0, sizeof(rfu_host.clients[i]));
            gpsp_rfu_link_down_hook(RFU_DOWN_TIMEOUT, i);
          }
        }
      }
    }
  }
}

void rfu_net_receive(const void* buf, size_t len, uint16_t client_id) {
  // RFU1 header, just some minor sanity check really.
  if (len >= 12 && upack32((const u8*)buf) == NET_RFU_HEADER) {
    u32 i, j;
    const u8 *buf8 = (const u8*)buf;
    const u8 *payl = &buf8[12];
    u32 ptype = upack32(&buf8[4]);
    u32 hdata = upack32(&buf8[8]);

    switch (ptype) {
    case NET_RFU_BROADCAST:
      // Fill the broadcast slot for the peer
      RFU_DEBUG_LOG("Got broadcast (client: #%d devID: %02x)\n", client_id, hdata);
      // 6 payload words are read below; the `len >= 12` gate above does not
      // cover them (same hardening as NET_RFU_CLIENT_SEND).
      if (client_id < MAX_RFU_PEERS && len >= 12 + 6 * 4) {
        rfu_peer_bcst[client_id].device_id = hdata;
        rfu_peer_bcst[client_id].valid = 1;
        rfu_peer_bcst[client_id].ttl = 0xff;
        for (j = 0; j < 6; j++)
          rfu_peer_bcst[client_id].data[j] = upack32(&payl[j*4]);
      }
      break;

    case NET_RFU_CONNECT_REQ:
      RFU_DEBUG_LOG("Received Conn Req (client ID: %d)\n", client_id);
      if (rfu_state == RFU_STATE_HOST) {
        // Ensure this client is not already connected!
        for (i = 0; i < 4; i++)
          if (rfu_host.clients[i].devid &&
              rfu_host.clients[i].client_id == client_id) {

            RFU_DEBUG_LOG("Connection request ignored: already connected!\n");
            return;
          }

        // Find an empty connection slot
        for (i = 0; i < 4; i++)
          if (!rfu_host.clients[i].devid) {
            u16 newid = new_devid();
            rfu_host.clients[i].devid = newid;
            rfu_host.clients[i].client_id = client_id;
            RFU_DEBUG_LOG("Connected client: assigned new devID %02x\n", newid);

            // Respond with ACK
            rfu_net_send_cmd(client_id, NET_RFU_CONNECT_ACK, newid | (i << 16));
            return;
          }
        
        // Not enough slots, NACK
        rfu_net_send_cmd(client_id, NET_RFU_CONNECT_NACK, 0);
      } else {
        RFU_DEBUG_LOG("Conn Req ignored, device is not in Host mode\n");
        rfu_net_send_cmd(client_id, NET_RFU_CONNECT_NACK, 0);
      }
      break;
      
    case NET_RFU_CONNECT_ACK:
      RFU_DEBUG_LOG("Received connection ACK from client ID: %d\n", client_id);
      // Only ok if we are not connected (not hosting)
      if (rfu_state == RFU_STATE_CONNECTING) {
        // Clear state and install device ID and slot number.
        memset(&rfu_client, 0, sizeof(rfu_client));
        rfu_client.devid = hdata & 0xffff;
        rfu_client.clnum = hdata >> 16;
        rfu_client.host_id = client_id;
        RFU_SET_STATE(RFU_STATE_CLIENT, RFU_TRC_CONN_ACK);
        RFU_DEBUG_LOG("Client connected with slot ID %d\n", rfu_client.clnum);
      }
      break;

    case NET_RFU_CONNECT_NACK:
      // When receiving a NACK just return to Idle state.
      if (rfu_state == RFU_STATE_CONNECTING)
        RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_CONN_NACK);
      RFU_DEBUG_LOG("Received CONN NACK\n");
      break;

    case NET_RFU_DISCONNECT:
      if (rfu_state == RFU_STATE_HOST) {
        // Clear the client from the list
        u32 clnum = (hdata >> 16) & 0x3;
        u16 cldid = hdata & 0xffff;
        if (rfu_host.clients[clnum].devid == cldid) {
          memset(&rfu_host.clients[clnum], 0, sizeof(rfu_host.clients[clnum]));
          gpsp_rfu_link_down_hook(RFU_DOWN_PEER, clnum);
        }
      }
      else if (rfu_state == RFU_STATE_CLIENT) {
        /* ADR-0074: measure the damage whether or not we are fixing it --
         * `queued` IS the number of exit-negotiation packets the historical
         * path threw away. */
        u32 queued = rfu_client_queued();
        gpsp_rfu_trace_hook(RFU_TR_DISCQ, queued, rfu_disc_defer);
        /* ADR-0076: grace window.  Stay CLIENT so the game can finish its
         * clock-master change and take the clean exit itself; see the block
         * comment at rfu_disc_grace.  mode=3 in the trace = grace armed. */
        if (rfu_disc_grace) {
          if (!rfu_disc_pending)
            gpsp_rfu_trace_hook(RFU_TR_DISCQ, queued, 3);
          rfu_disc_pending     = 1;
          rfu_disc_grace_armed = 1;
          rfu_disc_wait        = rfu_disc_grace;
          break;
        }
        if (rfu_disc_defer && queued) {
          rfu_disc_pending = 1;
          rfu_disc_wait    = RFU_DISC_DRAIN_MAX_FRAMES;
          break;            /* keep the queue; drain it first */
        }
        // Go back to idle state!
        {
          u32 clnum = rfu_client.clnum;
          memset(&rfu_client, 0, sizeof(rfu_client));
          RFU_SET_STATE(RFU_STATE_IDLE, RFU_TRC_DISC_PEER);
          gpsp_rfu_link_down_hook(RFU_DOWN_PEER, clnum);
        }
      }
      break;

    case NET_RFU_HOST_SEND:
      // Only possible if we are a client
      if (rfu_state == RFU_STATE_CLIENT) {
        u32 blen = hdata & 0x7f;
        if (len >= blen + 12) {
          u32 i;
          /* ADR-0060: a KEEPALIVE, not an acknowledgement of this packet.
           * The host only needs to know we still exist (clttl, 4 s); it does
           * not need a reply per packet, and NET_RFU_CLIENT_SEND already
           * refreshes it whenever we are actually talking. */
          if (rfu_frame_us - rfu_cli_last_tx_us >= RFU_CLIENT_KEEPALIVE_US)
            rfu_net_send_cmd_unrel(client_id, NET_RFU_CLIENT_ACK,
                                   rfu_client.devid | (rfu_client.clnum << 16));
          /* ADR-0072: CLUMPING, MEASURED DIRECTLY.
           *
           * This is the arrival of a real host packet at the client, before
           * any queueing and before the game polls for it — so unlike
           * RFU_TR_RXBURST (which counts RECV_DATA *commands* and is therefore
           * a poll-rate proxy, see rfu_frame_update) nothing here can be
           * confounded by how hard the game happens to be polling.
           *
           * The question is whether two host packets land in the SAME emulated
           * frame.  The game drains its queue once per frame, so an arrival
           * with a zero frame-delta is exactly one unit of queue growth — the
           * thing that would push depth toward pokeemerald's threshold of 4.
           * A steady one-per-frame stream, however fast, can never do it.
           *
           * clumped/total over a window is the fraction that matters, and it
           * is the number that has to come back non-trivial before the "we
           * deliver in clumps" half of ADR-0072 may be claimed again. */
          if (rfu_frame_no == rfu_arr_last_frame)
            rfu_arr_clumped++;
          rfu_arr_last_frame = rfu_frame_no;
          if (++rfu_arr_total >= RFU_ARRIVAL_CENSUS_N) {
            gpsp_rfu_trace_hook(RFU_TR_ARRIVAL, rfu_arr_clumped, rfu_arr_total);
            rfu_arr_clumped = 0;
            rfu_arr_total   = 0;
          }

          // Receive data from the host. Queue que packet if possible
          for (i = 0; i < RFU_PKT_QUEUE; i++) {
            if (!rfu_client.pkts[i].hblen) {
              memcpy(&rfu_client.pkts[i].hdata, payl, blen);
              rfu_client.pkts[i].hblen = blen;
              rfu_client.pkts[i].t_us  = rfu_frame_us;   /* ADR-0055 */
              rfu_client.pkts[i].arr_frame = rfu_frame_no; /* CUSHION aging */
              RFU_DEBUG_LOG("Recv host packet (%d bytes) Q[#%d]\n", blen, i);
              rfu_q_note(0, i + 1);
              return;
            }
          }
          RFU_DEBUG_LOG("Client dropped a host packet\n");
          gpsp_rfu_trace_hook(RFU_TR_QDROP, 0, rfu_client.clnum);
        }
      }
      break;

    case NET_RFU_CLIENT_SEND:
      // Only available when we are hosting
      if (rfu_state == RFU_STATE_HOST) {
        u32 i;
        u16 cdevid = hdata & 0xffff;
        u32 clid = (hdata >> 16) & 0x3;
        u32 blen = hdata >> 24;

        // Bounds-check the wire length before it reaches the memcpy below.
        // The sender caps this at 16 (see RFU_CMD_RTX_WAIT), but nothing on
        // THIS side did: the header byte allows 255 into a 16-byte buffer,
        // and unlike NET_RFU_HOST_SEND there was no `len >= blen + 12`
        // check either, so a malformed or foreign packet could both read
        // past the received buffer and smash the neighbouring client slots.
        if (blen > sizeof(rfu_host.clients[clid].pkts[0].data) ||
            len < blen + 12) {
          RFU_DEBUG_LOG("Drop oversized client packet (%d bytes)\n", blen);
          break;
        }

        // Validate the slot with device ID
        if (rfu_host.clients[clid].devid == cdevid) {
          rfu_host.clients[clid].clttl = 0;   // Account for packet reception
          for (i = 0; i < RFU_PKT_QUEUE; i++) {
            if (!rfu_host.clients[clid].pkts[i].datalen) {
              memcpy(rfu_host.clients[clid].pkts[i].data, payl, blen);
              rfu_host.clients[clid].pkts[i].datalen = blen;
              RFU_DEBUG_LOG("Recv client packet (%d bytes) Q[#%d]\n", blen, i);
              rfu_q_note(1, i + 1);
              return;
            }
          }
          RFU_DEBUG_LOG("Host dropped a client packet\n");
          gpsp_rfu_trace_hook(RFU_TR_QDROP, 1, clid);
        }
      }

    case NET_RFU_CLIENT_ACK:
      // Should only happen when hosting
      if (rfu_state == RFU_STATE_HOST) {
        u32 devid = hdata & 0xffff;
        u32 clid = (hdata >> 16) & 0x3;

        if (rfu_host.clients[clid].devid == devid)
          rfu_host.clients[clid].clttl = 0;   // Account for packet reception
      }
    };
  } else {
    RFU_DEBUG_LOG("Drop malformed packet\n");
  }
}

// Account for consumed cycles and return if a serial IRQ should be raised.
bool rfu_update(unsigned cycles) {
  /* ADR-0068: the HOST's mid-frame receive.  Same reentrancy contract as the
   * WAITEVENT poll below -- netpacket_poll_receive() can dispatch straight
   * back into rfu_net_receive(), which mutates rfu_comstate and friends -- so
   * rfu_comstate is deliberately re-read by the `if` that follows rather than
   * cached across the call. */
  if (rfu_idle_poll_cycles && rfu_comstate != RFU_COMSTATE_WAITEVENT) {
    rfu_idle_poll_acc += cycles;
    if (rfu_idle_poll_acc >= rfu_idle_poll_cycles) {
      rfu_idle_poll_acc = 0;
      netpacket_poll_receive();
    }
  }

  if (rfu_comstate == RFU_COMSTATE_WAITEVENT) {
    /* Force receive packets so that we can perhaps abort the wait.
     * This helps minimize latency (otherwise we need to wait a full
     * frame!).
     *
     * REENTRANCY: netpacket_poll_receive synchronously dispatches to
     * the frontend's netplay code, which may then call back into our
     * netpacket_receive -> rfu_net_receive on the same thread.
     * rfu_net_receive freely mutates rfu_state, rfu_host, rfu_client,
     * rfu_peer_bcst[] and other rfu_* globals.  All reads below must
     * re-fetch these globals after this point - do NOT cache rfu_state
     * etc. into a local across this call. */
    /* ADR-0062d: rate-limit this.  The reason it is here at all is latency
     * ("otherwise we need to wait a full frame!"), and one poll per 1/8
     * emulated frame preserves that eight times over while removing ~98 % of
     * the calls.  See rfu_set_poll_min_cycles(). */
    rfu_poll_acc += cycles;
    if (!rfu_poll_min_cycles || rfu_poll_acc >= rfu_poll_min_cycles) {
      rfu_poll_acc = 0;
      netpacket_poll_receive();
    }

    // Check if we are running our of time to respond.
    rfu_timeout_cycles -= MIN(cycles, rfu_timeout_cycles);
    rfu_resp_timeout   -= MIN(cycles, rfu_resp_timeout);

    // Wait for GBA to go into slave mode before finishing the wait!
    if ((read_ioreg(REG_SIOCNT) & 0x1) == 0) {
      if (rfu_state == RFU_STATE_IDLE) {
        // We are disconnected (most likely)
        rfu_buf[0] = 0x99660000 | (1 << 8) | RFU_CMD_RESP_DISC;
        rfu_buf[1] = 0xF;  // Reason disconnect (0), all slots disconnected?
        rfu_buf[2] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 3;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        /* ADR-0072: THE "PRESS A TO RETURN TO LOBBY" SCREEN, MADE OBSERVABLE.
         *
         * This answer -- RESP_DISC with every slot flagged -- is the only path
         * in this file that can yield RFU_STATUS_CONNECTION_ERROR, i.e. the
         * RECOVERABLE link error.  The `return -1` sites hooked as
         * RFU_TR_CMDERR are all the FATAL family (gWirelessCommType=3, whose
         * only exit is the power switch), so they cannot explain what the user
         * actually saw.  Until now this had NO HOOK AT ALL: the event has never
         * appeared in any log this project has captured, and re-reading old
         * logs will never find it.
         *
         * a = the slot mask we claim disconnected; b = the adapter state that
         * got us here, so a run can say WHY it went idle, not merely that it
         * did. */
        gpsp_rfu_trace_hook(RFU_TR_DISCANS, rfu_buf[1], rfu_state);
        RFU_DEBUG_LOG("Wait command resp: disconnect\n");
      }
      else if (rfu_data_avail()) {
        // Some event is available!
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_DATA;
        rfu_buf[1] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 2;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
      }
      else if (rfu_state == RFU_STATE_HOST && !rfu_resp_timeout) {
        // We "retransmitted" the message N times (not really, but the equivalent
        // time has elapsed) and we simulate a lack of client response.
        rfu_noresp_n++;   /* ADR-0059 */
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_DATA | (1 << 8);
        rfu_buf[1] = 0x00000F0F;  // TODO: just using 4 slots for now
        rfu_buf[2] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 3;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
      }
      else if (!rfu_timeout_cycles) {
        // We ran out of time, just return an "error" code
        rfu_timeo_n++;    /* ADR-0059 -- the central event of CLIFF-FINDINGS */
        rfu_buf[0] = 0x99660000 | RFU_CMD_RESP_TIMEO;
        rfu_buf[1] = 0x80000000;
        rfu_cnt = 0;
        rfu_plen = 2;
        rfu_comstate = RFU_COMSTATE_WAITRESP;
        RFU_DEBUG_LOG("Wait command resp: timeout\n");
      }
    }
  }

  if (rfu_comstate == RFU_COMSTATE_WAITRESP) {
    // Pushes command and data as an RFU-master back to the GBA.
    // Only if SO/SI are low and the device is active to receive stuff
    if ((read_ioreg(REG_SIOCNT) & 0xC) == 0 && (read_ioreg(REG_SIOCNT) & 0x80)) {
      // Write data into the "received" register and clear active bit.
      write_ioreg(REG_SIODATA32_H, rfu_buf[rfu_cnt] >> 16);
      write_ioreg(REG_SIODATA32_L, rfu_buf[rfu_cnt] & 0xffff);
      write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80));
      rfu_cnt++;
      // Go back to slave mode
      if (rfu_cnt == rfu_plen)
        rfu_comstate = RFU_COMSTATE_WAITCMD;
      return read_ioreg(REG_SIOCNT) & 0x4000;
    }
  }

  return false;
}

