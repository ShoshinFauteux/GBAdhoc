/* fe_priv.h -- main_psp.c's own macros (PLAN-P-beta5 §4 P5, the macro hoist).
 *
 * PRIVATE TO main_psp.o: main_psp.c includes it once, after its last top-of-file
 * project header (sidecar_dirs.h), where every condition below is final -- in
 * particular GPSP_NO_TELEMETRY, which fe_evt.h derives from GPSP_PLAYABLE.  Never
 * include it from another translation unit.
 *
 * Every #define here was moved verbatim from main_psp.c, in its old relative order,
 * re-wrapped under the condition it sat under there (both arms of one condition
 * together).  The objects and functions those conditions also hold stayed in
 * main_psp.c, at their lines; a macro whose expansion names one of them is still
 * legal, because a macro expands where it is used.
 *
 * Deliberately NOT here:
 *  - the core-ABI hand copies: main_psp.c's own declarations of core objects and
 *    functions.  They are declarations, not macros, and their order is the
 *    tentative-definition and .bss order, so they stay where they are.  Index, by
 *    name and in source order (grep the name: their line numbers move):
 *    at file scope
 *      core perf counters: flush_rom_total, flush_ram_full, flush_ram_smc,
 *        flush_ram_dma, gamepak_page_loads, smc_flush_us, smc_flush_bytes,
 *        smc_flush_wide, smc_blk_xlat_total, cph_jit_total, smc_blk_watch;
 *      translation-cache tier and heap hooks: jit_cache_tier,
 *        jit_cache_force_small, jit_cache_reason, rom_translation_cache_size,
 *        ram_translation_cache_size, rom_translation_cache,
 *        dynarec_release_translation_caches, dynarec_heap_tables_release,
 *        heap_trace_mark, gbcore_prepare, heap_trace_report;
 *      ROM-cache budget: gamepak_buffer_cap, gamepak_resident_wanted,
 *        gamepak_cap_reason, gamepak_force_swap_stubs;
 *      SMC and phase probes: smc_prof_take, smc_blk_take, smc_prof_clock,
 *        core_phase_take, core_phase_set_level;
 *      gba_screen_pixels;
 *      the ME renderer's view of GBA memory: vram, vram_clean, vram_dirty_marks,
 *        io_registers, oam_ram, palette_ram_converted, reg (with the
 *        VRAM_DIRTY_SHIFT / VRAM_DIRTY_PAGES constants beside them);
 *      skip_next_frame;
 *      ROM-cache stats: gamepak_size, gamepak_buffer_count, gamepak_buffer_pages,
 *        gamepak_buffers, gamepak_resident_got, gamepak_static_blocks,
 *        gamepak_must_swap, gamepak_vol_drop, gamepak_vol_back;
 *      iwram, ewram, palette_ram (the state hash);
 *      gpsp_rom_load_progress (above gba_reload_progress);
 *      gamepak_reopens, gamepak_short_reads, badjump_contained, dma_smc_flushes
 *        (telemetry builds only, above the heartbeat's xlat probes);
 *    at block scope
 *      smc_gates_reload_evt() and main(): translation_gate_targets,
 *        translation_gate_target_pc;
 *      rig_romload_evt(): gamepak_reopens, gamepak_short_reads;
 *      psp_rom_pool_offer(), psp_rom_pool_over(), psp_rewind_pool_back():
 *        gamepak_spare_pool3, gamepak_spare_pool3_bytes;
 *      main(): me_log_vram_always, gpsp_heap_census, gpsp_rom_load_progress.
 *    T14 asserts each one against the core's header in a compile-only check TU
 *    that is never linked: tools/refactor/abi_asserts.py ->
 *    tools/refactor/generated/<flavour>/{abi_check.c, fe_abi_asserts.h}.  Those
 *    files are the complete per-flavour list: they cite each copy's current line
 *    and also list, as UNCHECKED, the declarations with no core header to compare;
 *  - the macro-globals (g_start_keepalive/_grace/_heard, g_reload_slow_ms,
 *    g_gblink_host_wait_peer, g_mgift_consumed, g_mg_on, g_log_input, g_gba_late,
 *    g_turbo_lock_sim): in another arm each is an object, and the two stay together;
 *  - the macros a host test reads as text through main_psp.c's @extract markers
 *    (tools/tests: IO_JOIN_US, AUDIO_JOIN_US, g_wake_hold, the sleep-quiesce bounds,
 *    PWR_REQ_MAX_FIRES, SLEEP_HOLD_NAPS, the low-latency defaults, ...).
 */
#ifndef FE_PRIV_H
#define FE_PRIV_H

#include "play_defaults.h"   /* the PLAY_* ship constants and link defaults */

/* STALL WATCH breadcrumbs: the two stores into main_psp.c's g_loop_iter /
 * g_loop_phase, compiled out of a player build (see the comment there). */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
#define LOOP_TOP()      do { g_loop_iter++; g_loop_phase = 1; } while (0)
#define LOOP_PHASE(p)   do { g_loop_phase = (p); } while (0)
#else
#define LOOP_TOP()      do { } while (0)
#define LOOP_PHASE(p)   do { } while (0)
#endif

/* EVT ff_win counters: main_psp.c's g_ffw_drawn / g_ffw_gated (telemetry
 * builds only; a player build compiles the counter out). */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
#define FFW_DRAWN() (g_ffw_drawn++)
#define FFW_GATED() (g_ffw_gated++)
#else
#define FFW_DRAWN() ((void)0)
#define FFW_GATED() ((void)0)
#endif

/* `volatile` orders the two indices, not the samples: a compiler barrier
 * (no instruction) keeps the plain sample stores before the publish of
 * ring_w, and the sample loads after the level check and before the
 * release of ring_r.  Single CPU, so no hardware fence is needed. */
#define RING_BARRIER() __asm__ __volatile__("" ::: "memory")

/* The GE samples power-of-two textures only, and vid_image uses stride ==
 * texw, so the snapshot is stored 256 wide with the live 240x160 as its
 * top-left sub-rect.  The texture is DECLARED 256x256 (texh must be a power
 * of two), but only rows 0..160 are ever sampled: the picture is at most 160
 * rows and row src_h holds the bottom-edge copy for bilinear taps -- the
 * same 161 rows the ME/CPU staging buffer the live frame is drawn from has
 * (MER_STAGE_PITCH * 161), drawn by the same code (vid_image_screen).  So
 * only those 161 rows are stored: 82,432 B instead of 131,072. */
#define WAKE_TEX     256
#define WAKE_ROWS    161
#define WAKE_FRAME_BYTES (WAKE_TEX * WAKE_ROWS * 2)

/* The in-game menu's five state-slot thumbnails (main_psp.c: g_ui_thumbs, carved
 * from the netdrv arena's tail; its _Static_assert on the slot count stays there). */
#define UI_THUMB_BYTES (PSP_STATE_SLOT_COUNT * PSP_STATE_THUMB_SLOT_PIXELS * 2u)

/* THE EXIT MACROS (P3 retired the SDK-name hijack): every process exit and USB
 * handoff of main_psp.c goes through these, so no exit leaves the power switch
 * held (main_psp.c: the comment above the sleep simulator, harness.b's head).
 * pwr_release_all() is main_psp.c's; LAB_HANDOFF is lab_harness.h's. */
#define FE_HANDOFF(c,w) (pwr_release_all(), LAB_HANDOFF((c),(w),handoff_run))
#define FE_EXIT_GAME() (pwr_release_all(), sceKernelExitGame())

/* THE SLEEP SIMULATOR's hooks into the power code (harness only: main_psp.c's
 * g_ss_* and sleep_sim_*); without GPSP_PERF_RIG they are the real request
 * poll and constant 0s. */
#ifdef GPSP_PERF_RIG
#define SLEEP_SIM_ASLEEP() (g_ss_asleep)
#define SLEEP_SIM_NOPARK() (g_ss_nopark)
#define SLEEP_SIM_REQUEST() (g_ss_request)
#define POWER_IS_REQUEST() \
   (!g_ss_nopoll && (g_ss_req_up || scePowerIsRequest() == 1))
#else
#define SLEEP_SIM_NOPARK() 0
#define SLEEP_SIM_REQUEST() 0
#define POWER_IS_REQUEST() (scePowerIsRequest() == 1)
#define SLEEP_SIM_ASLEEP() 0
#endif

/* The ME renderer's hold on the volatile pool: through main_psp.c's vm_lock /
 * vm_unlock (the rewind shares the pool) when REWIND is built, else straight
 * to the kernel. */
#ifdef REWIND
#define MER_VM_TRYLOCK(b, s)   vm_lock((b), (s))
#define MER_VM_REJECT()        vm_unlock_engine()
#define MER_VM_UNLOCK()        vm_unlock()
#else
#define MER_VM_TRYLOCK(b, s)   sceKernelVolatileMemTryLock(0, (b), (s))
#define MER_VM_REJECT()        sceKernelVolatileMemUnlock(0)
#define MER_VM_UNLOCK()        sceKernelVolatileMemUnlock(0)
#endif

/* `inproc_me_unload = 1`, the harness MUTANT of the in-process return (main_psp.c:
 * g_inproc_me_unload and its comment); 0 in every other build. */
#ifdef GPSP_PERF_RIG
#define INPROC_ME_UNLOAD_MUTANT() (g_inproc_me_unload)
#else
#define INPROC_ME_UNLOAD_MUTANT() 0
#endif

/* The RFU trace ring is drained by main_psp.c's rfu_trace_drain() in a
 * telemetry build; without telemetry the call is nothing. */
#ifdef GPSP_NO_TELEMETRY
#define rfu_trace_drain() ((void)0)
#endif

/* fe_gblink.c NO_PEER_STEPS: the session's own wait for the partner's HELLO */
#define LINK_START_GRACE_US 30000000u

/* The rig's identity, audit and watch lines are functions of main_psp.c in a
 * telemetry build; a player build calls these no-ops instead.  (g_log_input,
 * the #define beside them there, is a macro-global: it stays.) */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
/* (this arm: no macro; main_psp.c defines the functions) */
#else
#define rig_preserve_log()          do { } while (0)
#define rig_boot_identity(h)        do { (void)(h); } while (0)
#define rig_rom_identity(p)         do { (void)(p); } while (0)
#define rig_cfg_audit_report()      do { } while (0)
#define rig_fail_probe()            do { } while (0)
#define rig_romload_evt(f)          do { (void)(f); } while (0)
#define rig_watch_evt(f)            do { (void)(f); } while (0)
#endif

/* The boot timeline (docs/UI-OVERLAY.md §12.2.1): main_psp.c's g_boot_mark[],
 * g_boot_t0, boot_mark() and boot_timeline_emit() in a telemetry build. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
#define BOOT_MARKS 12
#define BOOT_T0_SET() (g_boot_t0 = sceKernelGetSystemTimeWide())
#else
/* A release reports no boot timeline, so it records none either
 * (g_boot_mark: 192 B of .bss and an INI-stats call per milestone). */
#define BOOT_T0_SET()          ((void)0)
#define boot_mark(tag)         ((void)(tag))
#define boot_timeline_emit()   ((void)0)
#endif

#ifdef REWIND
/* The game is gone (fe_host_shutdown dropped every rewind pointer): the pool
 * has one tenant again, so a sleep in the browser can hand it back. */
#define PSP_REWIND_GAME_OVER() \
   (psp_rom_pool_over(), g_rw_on = g_rw_pool = g_rw_asleep = 0, psp_rewind_pwr())
#endif /* REWIND */

#endif /* FE_PRIV_H */
