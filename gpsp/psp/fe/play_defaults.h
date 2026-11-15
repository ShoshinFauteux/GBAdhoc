/* play_defaults.h -- the PLAY_* ship constants and the link defaults of main_psp.c
 * (PLAN-P-beta5 §4 P5), moved verbatim with their own comments, in their old order.
 *
 * PRIVATE TO main_psp.o: included by fe/fe_priv.h only.  The defaults a host test
 * reads as text stay in main_psp.c: PLAY_ME_LOWLAT and PLAY_ME_LATE_PRESENT
 * (tools/tests/test_lowlat_policy.py, region lowlat:defaults).
 * So does PLAY_INPUT_FRESH's default: only #define lines move, so its
 * #ifndef PLAY_INPUT_FRESH guard would stay there, testing the macro after this
 * header had defined it.  main_psp.c (fe/paths.inc.c) keeps a pointer comment
 * where the definitions sat.
 */
#ifndef PLAY_DEFAULTS_H
#define PLAY_DEFAULTS_H

/* ---- ADR-0067: THE PLAYABLE BUILD -------------------------------------
 *
 * `-DGPSP_PLAYABLE` produces a binary for someone to actually PLAY, from the
 * same tree as the harness build and with no runtime way to confuse the two:
 *
 *  * the harness ini path is pointed at a filename that cannot exist, so a
 *    leftover `.gpsp-harness.ini` on the user's card is inert.  No autopilot,
 *    no autoexit, no USB handoff, and the ROM browser comes up — none of which
 *    depends on the user having cleaned their card;
 *  * every harness-only key therefore returns its DEFAULT, so the shipping
 *    values below are supplied as those defaults rather than by a second code
 *    path.  There is exactly one place each knob is read, in both builds;
 *  * telemetry is compiled out (`GPSP_NO_TELEMETRY`), and no log directory or
 *    log file is created at all;
 *  * `PCFG_SESSION_FPS_DEF` is NOT touched.  The 29.97 default stands for the
 *    generic build; this build overrides its own copy of the setting.
 *
 * The four PLAY_* values are the FULLSPEED §28.2 ship stack.  Every
 * tools/build.sh profile is GPSP_PLAYABLE (harness, lab and soak add
 * GPSP_PERF_RIG, which reads the harness ini again), and psp/Makefile refuses
 * a build without it, so these are the only values: the bare-`make` flavour
 * that set all of them to 0 is gone (code-health A-5). */
#define PLAY_EMU_PRIO       0x2B
#define PLAY_IO_PRIO         0x2C
#define PLAY_POLL_MIN_CYCLES 34952
#define PLAY_BOOST_US        0
/* ADR-0068 ON for a player.  Measured over 15 alternating runs at 59.73 with
 * COMPLETE SEPARATION on both consoles (p = 0.0002): host srtt 2.02 -> 0.91
 * emulated frames, join 1.45 -> 0.70.  Mechanism verified every run -- host
 * mid-frame polls 0 without it, 7-8 with it.
 *
 * IT IS NOT FREE: host frames-over-budget rise ~6.5 points (median 9.49 ->
 * 16.03 %, fixed 7000-frame prefix, distributions overlapping), which in
 * practice is ~59.1 fps median falling to ~57.4 on the HOST only; the join is
 * unchanged.  Included anyway because this build exists to complete trades by
 * hand and the link-robustness evidence is far stronger than the frame cost.
 * Set to 0 for the extra ~1.7 fps on the host. */
#define PLAY_IDLE_POLL       34952
#define PLAY_DUMP_MARKER     0          /* ADR-0066: OFF for a player */
/* ADR-0075: VALIDATED on hardware (fs-pace pace arm — Gate A closed, send
 * queue held <=1, gHeldKeyCodeToSend never nulled, trades clean).  ON for
 * players: this is the input-eating fix. */
#define PLAY_RFU_FRAME_PACE  1
/* Fixed-depth cushion (jitter buffer) PoC: hold each host packet N frames so a
 * stall must drain the whole reserve before the client underruns.  OFF by
 * default — UNVALIDATED on hardware; the desktop cannot exercise it (injected
 * jitter starves rather than backlogs).  Set rfu_cushion=N in the harness ini
 * to A/B it.  Keep N well under RFU_DEF_TIMEOUT (32). */
#define PLAY_RFU_CUSHION     0
/* Link shedding: keep at most 2 frames of standing depth in each RFU receive
 * queue by removing content-free packets (rfu.c).  ON for players: this bounds
 * the round-trip latency that otherwise ratchets up for a whole session. */
#define PLAY_RFU_SHED_KEEP   2
/* Hold, never discard: a packet that does not fit its RFU receive queue waits
 * in an overflow ring instead of being thrown away (rfu.c rfu_hold).
 * ON for players since 3.0.0: PPSSPP radio fades, 5/5 survived with it and
 * 5/5 died without; on hardware (clean air, hold-ch11) it cost nothing, 4/4 on
 * the floor.  The noisy-air hardware arm was not run (owner's call, 2026-09-27;
 * docs/RIG-DOUBLE-BATTLE.md section 14).  The harness still sets it per arm. */
#define PLAY_RFU_HOLD        1
/* ADR-0076: NOT superseded after all.  Live-hardware 2026-08-10 (fs stage-live)
 * proved exit_assist alone leaves the JOIN at rfu_discq mode=4 (grace expired)
 * with the queue wiped and 1 discans (press-A) still.  disc_grace=60 drained
 * the undelivered exit packets 64->3 but expired 3 short; 120 reached mode=5 —
 * the game issues its own clean disconnect INSIDE the window, rcl=0x0101, zero
 * discans on BOTH consoles.  Safe: exit_assist pins ka=0 so the 61-frame
 * keepalive never fires during the wider window.  ON for players. */
#define PLAY_RFU_DISC_GRACE  120
/* ADR-0079: VALIDATED (run 317, re-confirmed live 2026-08-10 — both sides run
 * the game's own READY_CLOSE_LINK handshake, rcl=0x0101, card stamped).  ON for
 * players: this is the "press A to return to lobby" fix. */
#define PLAY_EXIT_ASSIST     1
/* ADR-0080c: the Media Engine video-staging offload.  ON for players — the
 * ~750us/frame the ME frees is the performance headroom (join proven: blit
 * stage 751->0us, 37,200 frames 0 drops, srtt champion-band, trades clean;
 * host offload via the session-gated activation that dodges np_start).
 * SELF-DISABLING: if the ME does not answer its boot handshake (250 ms) or
 * the watchdog trips, the console silently runs single-core — so a model
 * where the ME behaves differently degrades, it does not break.  Validated
 * on a PSP-1000 pair; the fallback covers the rest until more models are
 * tested. */
#define PLAY_ME_BOOT         1
#define PLAY_ME_VIDEO        1

/* The GBA link-cable session reserve taken at boot, in KiB (main_psp.c:
 * g_cable_reserve, and its comment says why and how it was sized).  Harness
 * cable_reserve_kb overrides it (0 = none, the old behaviour). */
#define CABLE_RESERVE_KB 2560u
/* Section 20: a cable session's default translation-cache offsets mod 8 KiB
 * (LAYOUT_PIN builds; -1 = keep the single-player pin).  hw6/hw7
 * (GBA-LINK-MIRROR.md 20.1-20.2): the LARGE tier (3000/Go) is fastest at
 * the pinned defaults; the SMALL tier (1000) with A at 0x1000 and B at
 * 0x0000 (-1.13 ms core vs the unpinned build).  Pair: AW2 59.8 fps,
 * Super Circuit 59.0 (unpinned 59.2 / 58.2). */
#define GBA_SESS_PIN_A_SMALL 0x1000
#define GBA_SESS_PIN_A_LARGE (-1)
#define GBA_SESS_PIN_B_SMALL 0x0000
#define GBA_SESS_PIN_B_LARGE (-1)

/* LINK SPEED: what a player's build runs with (docs/GBA-LINK-MIRROR.md 21-28;
 * 4.0.0 beta).  Each has a config.ini key of the same name as its kill
 * switch (0 = the path every build up to 3.5.0 ran), and the harness key of
 * that name overrides it for an A/B arm.  A harness build has the SAME
 * defaults: it must be the emulator the player gets.
 *   gba_link_fast = 3            cable fast path + lean hooks (link only)
 *   gba_link_headless_audio = 2  the partner mixes no sound, ring not read
 *   sched_fast = 1               update_gba skips the interrupt check when
 *                                IE & IF == 0 (SCHED_FAST builds; every game)
 *   irq_io_fast = 1              IE/IF/IME stores inline in the I/O store
 *                                stub (IRQ_IO_FAST builds; every game)
 *   msr_fast = 1                 CPU mode switches (MSR, SPSR restore)
 *                                banked in asm (MSR_FAST builds; every game)
 *   nd_ack_ride_us = 30000       acks ride on input packets: a GBA Link
 *                                Cable session, from RUNNING to its close
 *                                (netpacket_host.c fe_np_ack_ride_session) */
#define PLAY_GBA_LINK_FAST           3
#define PLAY_GBA_LINK_HEADLESS_AUDIO 2
#define PLAY_SCHED_FAST              1
#define PLAY_IRQ_IO_FAST             1
#define PLAY_MSR_FAST                1  /* MSR_FAST builds: mode switches in asm */
#define PLAY_ND_ACK_RIDE_US          30000
/* One pump period: the transport is serviced once per emulated frame, so an
 * ack that has waited out its ride leaves up to this much later.  The ride
 * is clamped to the RTO floor minus this (and minus nothing for transit,
 * which the adaptive RTO already contains). */
#define ND_PUMP_PERIOD_US 17000
/* The RTO floor a 0 in nd_rto_min_us means (netdrv's build-time default). */
#define ND_RTO_FLOOR_DEFAULT_US 30000

#endif /* PLAY_DEFAULTS_H */
