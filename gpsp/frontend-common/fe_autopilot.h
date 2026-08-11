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
 *   holdptr BTNS SZ PTR OFF MASK VAL TO   hold until deref-predicate holds
 *   holdmash HBTNS MBTNS SZ ADDR MASK VAL TO      hold HBTNS every frame and
 *                                   pulse MBTNS (2 on / 6 off) on top, until
 *                                   the predicate holds.  Movement workhorse:
 *                                   "holdmash RIGHT B ..." keeps walking and
 *                                   the B pulses dismiss step-triggered
 *                                   interruptions (Emerald Pokénav match
 *                                   calls) without ever talking to anything.
 *   holdmashptr HBTNS MBTNS SZ PTR OFF MASK VAL TO  ... deref variant
 *   waitsram TO                     wait until the 128 KiB SRAM CRC differs
 *                                   from its value when this step started
 *   mashsram BTNS TO                ... same, mashing BTNS while waiting
 *   logram NAME SZ ADDR             emit "EVT ap_val name=NAME val=0x..."
 *   logptr NAME SZ PTRADDR OFF      deref u32 GBA pointer at PTRADDR, read
 *                                   SZ bytes at target+OFF, emit ap_val
 *   repeat N / endrepeat            repeat the enclosed block N times
 *                                   (no nesting)
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

#ifdef __cplusplus
}
#endif

#endif /* FE_AUTOPILOT_H */
