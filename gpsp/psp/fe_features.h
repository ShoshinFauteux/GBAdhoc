/* psp/fe_features.h -- the frontend's half of gpsp_features.h.
 *
 * psp/Makefile force-includes this into every frontend object, so the values
 * that are the same in every PSP build need no recipe.  Keep it to comments and
 * preprocessor lines (assembler objects get it too).  It does not reach the core
 * archive or the Media Engine module, and nothing here reads a core feature:
 * core features reach the frontend as weak symbols, never as defines.
 *
 * What is NOT here: anything that changes what is compiled or linked (REWIND,
 * GBA_LINK, CHEATS, LAB_HARNESS, LAYOUT_PIN, LAYOUT_PAD_BYTES, HOT_DATA_GUARD),
 * anything that must reach a sub-make (PSP_PIXFMT), the GB core's scoped TGB_*
 * switches, and what makes a profile that profile (GPSP_PLAYABLE, GPSP_PERF_RIG,
 * GPSP_KEEP_TELEMETRY, GPSP_PROFILE_*: tools/build.sh).
 *
 * A -D on the compile line wins over a value here, which is also how a bench
 * changes one (FE_EXTRA="-DND_WINDOW=128").  -DGPSP_NO_NAME turns a playable
 * default off.  tools/build.sh records what the compiler resolved in the
 * manifest's frontendDefines; tools/check_flags.py holds this list against
 * tools/flags.lock.
 */
#ifndef FE_FEATURES_H
#define FE_FEATURES_H

/* ---- the PSP ad-hoc transport profile ----------------------------------------
 *
 * ND_* knobs -- the PSP/ad-hoc profile (ADR-0016, ADR-0017).  Two changes
 * from the Phase-4 halving, both driven by the 2026-08-01 field logs
 * (docs/HANDOFF.md issue #2):
 *
 *  * ND_MAX_PAYLOAD 560 -> 144.  ADR-0003 sized frames for the link-CABLE
 *    Advance Wars protocol (~520 B), which is out of scope; RFU -- all this
 *    product carries -- maxes at 104 B and the largest control payload is the
 *    138 B roster.  Right-sizing the slot pays for a 4x deeper tx ring
 *    (96 -> 384) and a 2x window at NEUTRAL memory: the netdrv instance goes
 *    ~322 KiB -> ~347 KiB while the transport's .bss ring drops ~37 -> ~11 KiB.
 *    An oversize payload is refused loudly, never truncated (netdrv.h).
 *  * RTO floor 30 ms -> 100 ms, ceiling 240 -> 800 ms.  Real ad-hoc RTT is
 *    40-80 ms; the loopback-tuned 30 ms floor fired before an ACK could
 *    physically arrive (measured retx/acked 2.8, 68 % duplicates, zero radio
 *    loss).  The floor only bounds the adaptive estimator from below.
 *
 * ND_RTO_MAX_US: THE CEILING MUST NOT SIT BELOW WHAT THE ESTIMATOR COMPUTES.
 *
 * At 59.73 fps the client measured srtt=495ms rttvar=166ms, so RFC 6298 wants
 * an RTO of srtt + 4*rttvar = ~1157ms.  The ceiling was 800ms, so EVERY packet
 * was guaranteed to time out before its acknowledgement could arrive -- a
 * retransmit storm by construction, not by loss.  Measured: retx_pct 421% on
 * the client, 360% on the host, txq_hi 336.
 *
 * 2500000 leaves headroom above the ~1157ms observed without being unbounded;
 * the estimator still governs, this only stops the clamp from lying.  Note the
 * floor stays at 200000: the problem was never that we resend too slowly.
 *
 * This does NOT explain retx_age of 3063ms (host) against a 200ms RTO -- that
 * is a separate fault, the ARQ scan not running, and is tracked separately.
 *
 * ND_WINDOW 128 -> 256: does the window STALL, or is it simply too small?
 *
 * At 59.73 the host reached txq_hi 886 against a 128-frame window -- a backlog
 * 7x deeper than the window can hold in flight -- while pumpgap measured 13-16ms
 * mean (the ARQ scan runs fine) and retx_age still hit 1.8-2.1 SECONDS. Two
 * explanations fit: the window stalls head-of-line on one unacknowledged packet
 * (cumulative-only ACK, no SACK), or it is just too narrow for full-speed
 * traffic. Doubling it separates them for one line instead of implementing
 * selective acknowledgement on a guess:
 *   retx_age and txq_hi collapse  -> the window was too small, keep this
 *   both unchanged                -> head-of-line blocking, SACK is the fix
 *
 * NOT FREE: ND_WINDOW sizes nd_rxslot reorder[ND_WINDOW] (netdrv.c:211), so
 * this also doubles the per-peer reorder buffer. Watch EVT mem_free.
 */
#ifndef ND_MAX_PAYLOAD
#define ND_MAX_PAYLOAD 144
#endif
#ifndef ND_WINDOW
#define ND_WINDOW 256
#endif
#ifndef ND_TXQ_CAP
#define ND_TXQ_CAP 384
#endif
#ifndef ND_RTO_MIN_US
#define ND_RTO_MIN_US 200000
#endif
#ifndef ND_RTO_MAX_US
#define ND_RTO_MAX_US 2500000
#endif

/* ---- defaults of a playable build (every profile but the bare e2e harness) ---
 *
 * VID_TRIPLE: triple buffering (psp/video_psp.c).
 * PLAY_INPUT_FRESH: the low-latency pad re-peek on by default
 * (docs/LOW-LATENCY.md); a playable build cannot read the harness ini.
 * INPUT_FRESH=0 on tools/build.sh is -DGPSP_NO_PLAY_INPUT_FRESH.
 */
#ifdef GPSP_PLAYABLE
#if !defined(GPSP_NO_VID_TRIPLE) && !defined(VID_TRIPLE)
#define VID_TRIPLE 1
#endif
#if !defined(GPSP_NO_PLAY_INPUT_FRESH) && !defined(PLAY_INPUT_FRESH)
#define PLAY_INPUT_FRESH 1
#endif
#endif /* GPSP_PLAYABLE */

#endif /* FE_FEATURES_H */
