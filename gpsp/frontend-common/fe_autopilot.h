/* fe_autopilot.h — autopilot input-script engine (plan §7.1 step 3).
 *
 * Executes a line-oriented script of frame-stamped button events interleaved
 * with RAM-predicate sync points, so harness runs are event-driven instead of
 * blind-timed (risk-register row 11).  RAM access goes through
 * fe_host_mem_read (EWRAM via retro_get_memory_data(SYSTEM_RAM), IWRAM via
 * the SET_MEMORY_MAPS descriptors — FRONTEND-AUDIT §6); input is injected
 * through fe_host_input_inject.  Game addresses live in the scripts, not
 * here — see docs/AUTOPILOT.md for the verified Emerald (BPEE rev 0) table.
 *
 * Script grammar (one command per line; '#'/';' comments; numbers decimal or
 * 0x-hex; BTNS = '+'-joined A B L R UP DOWN LEFT RIGHT START SELECT):
 *   evt TEXT                        emit "EVT ap_mark text=TEXT"
 *   ff on|off                       request fast-forward (frontends honor it)
 *   dump                            request a framebuffer BMP dump
 *   wait N                          idle N frames
 *   press BTNS [N]                  hold BTNS N frames (default 2), then
 *                                   release for 2 frames (guarantees JOY_NEW)
 *   hold BTNS N                     hold BTNS exactly N frames
 *   waitram   SZ ADDR MASK VAL TO   wait until (mem[ADDR]&MASK)==VAL, SZ in
 *                                   {1,2,4}, fail after TO frames
 *   waitramne SZ ADDR MASK VAL TO   ... until != VAL
 *   mash BTNS SZ ADDR MASK VAL TO   press/release BTNS (2 on / 6 off) until
 *                                   (mem[ADDR]&MASK)==VAL
 *   mashne BTNS SZ ADDR MASK VAL TO ... mash until (mem[ADDR]&MASK)!=VAL
 *   holdram BTNS SZ ADDR MASK VAL TO  hold BTNS continuously until
 *                                   (mem[ADDR]&MASK)==VAL (movement predicates:
 *                                   tap=turn, hold=walk in gen-3 overworld)
 *   waitptr   SZ PTR OFF MASK VAL TO  deref u32 GBA pointer at PTR, wait until
 *                                   (mem[*PTR+OFF]&MASK)==VAL.  Needed for
 *                                   Emerald's relocating gSaveBlock1Ptr and
 *                                   heap-allocated state blocks; an invalid/
 *                                   NULL pointer just evaluates false.
 *   waitptrne SZ PTR OFF MASK VAL TO  ... until != VAL
 *   mashptr BTNS SZ PTR OFF MASK VAL TO   mash until deref-predicate holds
 *   mashptrne BTNS SZ PTR OFF MASK VAL TO ... until it no longer holds
 *   holdptr BTNS SZ PTR OFF MASK VAL TO   hold until deref-predicate holds
 *   holdmash HBTNS MBTNS SZ ADDR MASK VAL TO      hold HBTNS every frame and
 *                                   pulse MBTNS (2 on / 6 off) on top, until
 *                                   the predicate holds.  Movement workhorse:
 *                                   "holdmash RIGHT B ..." keeps walking and
 *                                   the B pulses dismiss step-triggered
 *                                   interruptions (Emerald Pokénav match
 *                                   calls) without ever talking to anything.
 *   holdmashptr HBTNS MBTNS SZ PTR OFF MASK VAL TO  ... deref variant
 *   mashif BTNS CSZ CADDR CMASK CVAL SZ ADDR MASK VAL TO
 *                                   mash BTNS only on frames where
 *                                   (mem[CADDR]&CMASK)==CVAL, until
 *                                   (mem[ADDR]&MASK)==VAL (dialogue: press
 *                                   only while text prints, stop at the menu)
 *   waitsram TO                     wait until the 128 KiB SRAM CRC differs
 *                                   from its value when this step started
 *   mashsram BTNS TO                ... same, mashing BTNS while waiting
 *   logram NAME SZ ADDR             emit "EVT ap_val name=NAME val=0x..."
 *   logptr NAME SZ PTRADDR OFF      deref u32 GBA pointer at PTRADDR, read
 *                                   SZ bytes at target+OFF, emit ap_val
 *   repeat N / endrepeat            repeat the enclosed block N times
 *                                   (no nesting)
 *   state                           reload the savestate.  HARNESS BUILDS ONLY
 *                                   (-DGPSP_PERF_RIG).  The frontend hotkey that
 *                                   loads a state is disabled while a script
 *                                   runs, so a script cannot reach it any other
 *                                   way.
 *
 *   stepram BTNS SZ ADDR MASK VAL TO  walk one tile at a time: hold BTNS
 *                                   12 frames, release, re-check after 48;
 *                                   until the predicate holds.  Never
 *                                   overshoots when buttons arrive late
 *                                   (GB link sessions' input delay)
 *   ifram SZ ADDR MASK VAL N        run the next N steps only if
 *                                   (mem[ADDR]&MASK)==VAL now, else skip
 *                                   them (zero frames; a read error skips)
 *   logbytes NAME N ADDR             emit N (<=24) raw bytes as hex:
 *                                   "EVT ap_val name=NAME hex=..."
 * Every ap_mark, ap_sync, ap_val and ap_fail carries it=<repeat iteration,
 * 1-based, 0 outside a repeat>; ap_mark/ap_sync carry f= and t_ms=;
 * ap_loaded carries crc= (CRC32 of the script file); ap_fail carries val=
 * (the last value the failing predicate read, or ERR).
 *
 * EVT interface (grep-stable): ap_loaded, ap_sync (predicate satisfied),
 * ap_val, ap_mark, ap_done, ap_fail.
 */
#ifndef FE_AUTOPILOT_H
#define FE_AUTOPILOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a script file. Returns 0 on success (engine becomes active),
 * -1 on open/parse error (logged; engine stays inactive). */
int fe_autopilot_load(const char *path);

/* 1 if a script is loaded and not yet finished/failed. */
int fe_autopilot_active(void);

/* Advance the engine one frame. Call once per emulated frame BEFORE
 * fe_host_run_frame(); it evaluates predicates against the RAM state left
 * by the previous frame and injects this frame's pad input. */
void fe_autopilot_frame(void);

/* 0 = running, 1 = done (all steps passed), -1 = failed (predicate timeout),
 * 2 = no script loaded. */
int fe_autopilot_status(void);

/* 1 while the script has requested fast-forward ("ff on"). */
int fe_autopilot_ff(void);

/* Returns 1 (and clears the flag) if the script requested a frame dump. */
int fe_autopilot_dump_pending(void);

#ifdef GPSP_PERF_RIG
/* Returns 1 (and clears the flag) if the script requested a savestate RELOAD
 * (`state`).  Harness builds only, so the release ELF stays byte-identical:
 * code motion alone costs 0.5-1% of frame budget on this hardware, and release
 * can never run a script anyway.
 *
 * The host calls fe_host_state_load() on its own state path.  Scripts use this
 * to stress the reload path -- restoring repeatedly mid-battle is what exposes
 * dynarec or SMC state that survived a restore when it should not have. */
int fe_autopilot_state_pending(void);
/* `disconnect`: the script asks to end the GB link session (the menu's
 * Disconnect); the game plays on from where it stands afterwards. */
int fe_autopilot_disconnect_pending(void);

/* The text of the last `evt` step executed, or NULL; clears it (one-shot). */
const char *fe_autopilot_take_mark(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FE_AUTOPILOT_H */
