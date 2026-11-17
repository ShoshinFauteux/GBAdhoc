/* fe_priv.h -- the macros P5 hoisted out of main_psp.c (PLAN-P-beta5 §4 P5, the macro hoist).
 *
 * "main_psp.c" in this file means the translation unit: the shell psp/main_psp.c plus the
 * fe/X.inc.c fragments it includes, in include order ("above"/"below" follow that order).
 * psp/fe/README.md (generated from the cutlist) maps fragments to modules and lists where
 * each definition sits.
 *
 * PRIVATE TO main_psp.o: the shell includes it once, after its last top-of-file project
 * header (sidecar_dirs.h), where every condition below is final -- in particular
 * GPSP_NO_TELEMETRY, which fe_evt.h derives from GPSP_PLAYABLE.  Never include it from
 * another translation unit.
 *
 * Every #define here was moved verbatim from main_psp.c, in its old relative order,
 * re-wrapped under the condition it sat under there (both arms of one condition
 * together).  The objects and functions those conditions also hold stayed where they
 * were (the note above each block names the fragment); a macro whose expansion names one
 * of them is still legal, because a macro expands where it is used.
 *
 * NOT every TU-local macro: this file is P5's inventory only.  About 120 #define lines
 * stay in the shell and the fragments, at their definition points (psp/fe/README.md
 * counts them):
 *  - the macro-globals (g_start_keepalive/_grace/_heard, g_reload_slow_ms,
 *    g_gblink_host_wait_peer, g_mgift_consumed, g_mg_on, g_log_input, g_gba_late,
 *    g_turbo_lock_sim): in another arm each is an object, and the two stay together;
 *  - the macros a host test reads as text through an @extract marker (tools/tests:
 *    IO_JOIN_US, AUDIO_JOIN_US, g_wake_hold, the sleep-quiesce bounds, PWR_REQ_MAX_FIRES,
 *    SLEEP_HOLD_NAPS, the low-latency defaults, ...);
 *  - release no-op stubs like the rig_* ones below that P5's inventory did not list: they
 *    stay in the #else arm beside the telemetry function they replace (stall_watch_start/
 *    _stop, drprof_start/_stop/_window, DRPROF_PHASE, ff_win_note, UISCRIPT_DUMP:
 *    fe/harness.f.inc.c, which repeats them in the #else of its rig/telemetry wrap;
 *    core_health_evt's is here since C2-2);
 *  - local helpers and constants defined beside their users.
 * So a macro that is not here is not necessarily a macro-global or one a test reads.
 *
 * Also NOT here: the core-ABI hand copies, main_psp.c's own declarations of core objects
 * and functions.  They are declarations, not macros, and their order is the
 * tentative-definition and .bss order, so they stay where they are.
 *
 * The index below is PLAN-P-beta5 §5.2's set, NOT every hand copy: the names T14 asserts
 * (in any flavour) plus six it cannot assert.  T14 lists the copies itself, per flavour and
 * in no flavour (see the end of this comment).  By name, in TU order, with the file that
 * holds each group (grep the name for its line):
 *    at file scope
 *      fe/input.a.inc.c  core perf counters: flush_rom_total, flush_ram_full,
 *        flush_ram_smc, flush_ram_dma, gamepak_page_loads, smc_flush_us, smc_flush_bytes,
 *        smc_flush_wide, smc_blk_xlat_total, cph_jit_total, smc_blk_watch;
 *        translation-cache tier and heap hooks: jit_cache_tier, jit_cache_force_small,
 *        jit_cache_reason, rom_translation_cache_size, ram_translation_cache_size,
 *        rom_translation_cache, dynarec_release_translation_caches,
 *        dynarec_heap_tables_release, heap_trace_mark, gbcore_prepare, heap_trace_report;
 *      fe/inproc.a.inc.c  ROM-cache budget: gamepak_buffer_cap, gamepak_resident_wanted,
 *        gamepak_cap_reason, gamepak_force_swap_stubs;
 *      fe/input.b.inc.c  SMC and phase probes: smc_prof_take, smc_blk_take,
 *        smc_prof_clock, core_phase_take, core_phase_set_level;
 *      fe/merend.a.inc.c  gba_screen_pixels;
 *      fe/display.a.inc.c  the ME renderer's view of GBA memory: vram, vram_clean,
 *        vram_dirty_marks, io_registers, oam_ram, palette_ram_converted, reg (with the
 *        VRAM_DIRTY_SHIFT / VRAM_DIRTY_PAGES constants beside them);
 *      fe/merend.c.inc.c  skip_next_frame;
 *      fe/merend.d.inc.c  ROM-cache stats: gamepak_size, gamepak_buffer_count,
 *        gamepak_buffer_pages, gamepak_buffers, gamepak_resident_got,
 *        gamepak_static_blocks, gamepak_must_swap, gamepak_vol_drop, gamepak_vol_back;
 *      fe/misc.a.inc.c  iwram, ewram, palette_ram (the state hash);
 *      fe/link.d.inc.c  gpsp_rom_load_progress (above gba_reload_progress);
 *      fe/harness.f.inc.c  gamepak_reopens, gamepak_short_reads, badjump_contained,
 *        dma_smc_flushes (telemetry builds only, above the heartbeat's xlat probes);
 *    at block scope
 *      fe/harness.f.inc.c  smc_gates_reload_evt(): translation_gate_targets,
 *        translation_gate_target_pc; rig_romload_evt(): gamepak_reopens,
 *        gamepak_short_reads;
 *      fe/rewind.inc.c  psp_rom_pool_offer(), psp_rom_pool_over(),
 *        psp_rewind_pool_back(): gamepak_spare_pool3, gamepak_spare_pool3_bytes;
 *      main_psp.c (the shell), main(): me_log_vram_always, gpsp_heap_census,
 *        gpsp_rom_load_progress, translation_gate_targets, translation_gate_target_pc,
 *        and gamepak_reopens (GPSP_STANDBY_NOTES builds only).
 *    T14 (tools/refactor/abi_asserts.py) asserts a copy only when its check TU's headers
 *    declare the name.  That check TU (tools/refactor/generated/<flavour>/abi_check.c,
 *    compile-only, never linked) includes only common.h (the core's header chain) and
 *    psp/sleep_q.h.  generated/<flavour>/fe_abi_asserts.h lists every copy in that
 *    flavour's -E (release, lab, harness), cited as fe/X.inc.c:N (main_psp.c:N in the
 *    shell) of the tree it was generated on: the asserted ones, then the UNCHECKED list,
 *    the copies of that -E that T14 does not assert.  Most UNCHECKED names are not in this
 *    index: the rfu_set_* and adhoc_set_disc_await setters (fe/link.c.inc.c), sched_fast,
 *    irq_io_fast, msr_fast, timer_lazy, gpsp_wallclock and gpsp_rtc_peek (the shell),
 *    gbadhoc_net_touched, badjump_recoveries and smc_fallback_stats (fe/inproc.b.inc.c),
 *    gpsp_rtc_wall (fe/power.a.inc.c), gbab_gamepak_page_loads (fe/link.e.inc.c), and
 *    others.  Nothing checks those copies, so they carry the most risk.
 *    The six of this index that are UNCHECKED: dynarec_heap_tables_release, heap_trace_mark,
 *    heap_trace_report, gamepak_buffers and badjump_contained, which no header
 *    declares, and gbcore_prepare, which gbcore/gbcore.h declares but the check TU does not
 *    include.
 *    main()'s gamepak_reopens (GPSP_STANDBY_NOTES) is one of six copy sites in #if arms
 *    that none of release, lab and harness compiles, so no generated header lists them and
 *    nothing checks them; the others are gbab_reg and fe_gbadual_running_inst
 *    (fe/harness.f.inc.c, DRPROF_HW), rom_hash_census (fe/harness.f.inc.c,
 *    ROM_HASH_CENSUS), jit_pub_inline and xlat_scan_fast (fe/inproc.a.inc.c,
 *    JIT_COST_KEYS).  `abi_asserts.py unbuilt --src gpsp` lists them from a raw scan of the
 *    TU (every #if arm), with their lines; psp/fe/README.md prints that list.
 */
#ifndef FE_PRIV_H
#define FE_PRIV_H

#include "play_defaults.h"   /* the PLAY_* ship constants and link defaults */

/* HARNESS READS (tier C, C2-1; not from P5's hoist).  The one call form of a harness-ini read:
 * no other frontend code names HARNESS_INI in a getter (ci_greps.py R7; the one whitelisted
 * raw read is the frame loop's rewind_gedump, ci_greps.known).  Every key, its default and
 * the flavours that compile it are in tools/refactor/generated/harness_keys.tsv, which the
 * Lab validates staged keys against.
 * HARNESS_FOLD 0 (every build today): exactly the old call, so the token stream is unchanged.
 * HARNESS_FOLD 1 (C2-3, player builds only): each read is its default, as ADR-0067's
 * never-opened harness path already makes it at run time; HARNESS_STR leaves `out` alone,
 * as fe_ini_get on a never-opened path does.  A rig build never folds. */
#ifndef HARNESS_FOLD
#define HARNESS_FOLD 0
#endif
#if HARNESS_FOLD && defined(GPSP_PERF_RIG)
#error "HARNESS_FOLD folds the harness reads of a rig build (GPSP_PERF_RIG)"
#endif
#if !HARNESS_FOLD
#define HARNESS_INT(key, def)      fe_ini_get_int(HARNESS_INI, key, def)
#define HARNESS_STR(key, out, n)   fe_ini_get(HARNESS_INI, key, out, n)
#define HARNESS_PRESENT()          file_exists(HARNESS_INI)
#else
#define HARNESS_INT(key, def)      ((void)sizeof(key), (long)(def))
#define HARNESS_STR(key, out, n)   ((void)sizeof(key), (void)(out), (void)(n), 0)
#define HARNESS_PRESENT()          0
#endif

/* core_health_evt (tier C, C2-2): the soak line, defined in fe/harness.f.inc.c in a telemetry
 * build.  The state-load and reset functions of fe/misc.c.inc.c call it ABOVE that definition,
 * so a telemetry build declares it here; a player build gets the no-op. */
#ifdef GPSP_KEEP_TELEMETRY
static void core_health_evt(const char *at);
#else
#define core_health_evt(at) do { } while (0)
#endif

/* STALL WATCH breadcrumbs: the two stores into g_loop_iter / g_loop_phase
 * (fe/harness.a.inc.c), compiled out of a player build (see the comment there). */
#if defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
#define LOOP_TOP()      do { g_loop_iter++; g_loop_phase = 1; } while (0)
#define LOOP_PHASE(p)   do { g_loop_phase = (p); } while (0)
#else
#define LOOP_TOP()      do { } while (0)
#define LOOP_PHASE(p)   do { } while (0)
#endif

/* TEL(stmt): a statement that only feeds telemetry (an EVT line, the Lab), e.g.
 * TEL(g_ffw_drawn++), the EVT ff_win counters (fe/power.a.inc.c), or
 * TEL(pacing_tel.ll_hit++).  It names the statement as telemetry; it runs in EVERY
 * build.  Owner decision 2026-11-17: the Lab can run only Lab builds, so the code
 * that runs every frame stays the same in the player build and the Lab build, or
 * the Lab's speed and picture-hash results stop proving the shipped build; the
 * per-frame telemetry is not compiled out of a player build (what fe_evt reports,
 * and the census's EVT lines, still are: ADR-0067b).  Tier C C-5
 * (builds/health-out/design/C-5.md §2.1).  Variadic so a statement with a
 * top-level comma needs no extra parentheses. */
#define TEL(...) do { __VA_ARGS__; } while (0)

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

/* The in-game menu's five state-slot thumbnails (g_ui_thumbs, fe/display.a.inc.c,
 * carved from the netdrv arena's tail; its _Static_assert on the slot count stays there). */
#define UI_THUMB_BYTES (PSP_STATE_SLOT_COUNT * PSP_STATE_THUMB_SLOT_PIXELS * 2u)

/* THE EXIT MACROS (P3 retired the SDK-name hijack): every process exit and USB
 * handoff of main_psp.c goes through these, so no exit leaves the power switch
 * held (the comment above the sleep simulator, fe/harness.b.inc.c's head).
 * pwr_release_all() is fe/power.b.inc.c's; LAB_HANDOFF is lab_harness.h's. */
#define FE_HANDOFF(c,w) (pwr_release_all(), LAB_HANDOFF((c),(w),handoff_run))
#define FE_EXIT_GAME() (pwr_release_all(), sceKernelExitGame())

/* THE SLEEP SIMULATOR's hooks into the power code (harness only: the g_ss_* and
 * sleep_sim_* of fe/harness.b.inc.c); without GPSP_PERF_RIG they are the real
 * request poll and constant 0s. */
#ifdef GPSP_PERF_RIG
#define SLEEP_SIM_ASLEEP() (g_ss_asleep)
#define SLEEP_SIM_NOPARK() (g_ss_nopark)
#define SLEEP_SIM_REQUEST() (g_ss_request)
#define POWER_IS_REQUEST() \
   (!g_ss_nopoll && (g_ss_req_up || scePowerIsRequest() == 1))
/* `sleep_sim_me_sync_fail = 1` (PLAN S-9 mutant): the ME's cache-flush sync
 * fails while the simulated suspend is under way (me_rend_release_vmem). */
#define SLEEP_SIM_ME_SYNC_FAIL() (g_ss_me_sync_fail && g_ss_asleep)
/* After a failed sync's pool release: is the engine quiet?  (PLAN S-9's
 * measurement; sleep_sim_me_quiet, fe/harness.b.inc.c.) */
#define SLEEP_SIM_ME_QUIET() sleep_sim_me_quiet()
#else
#define SLEEP_SIM_NOPARK() 0
#define SLEEP_SIM_REQUEST() 0
#define POWER_IS_REQUEST() (scePowerIsRequest() == 1)
#define SLEEP_SIM_ASLEEP() 0
#define SLEEP_SIM_ME_SYNC_FAIL() 0
#define SLEEP_SIM_ME_QUIET() ((void)0)
#endif

/* The ME renderer's hold on the volatile pool: through vm_lock / vm_unlock
 * (fe/merend.b.inc.c; the rewind shares the pool) when REWIND is built, else
 * straight to the kernel. */
#ifdef REWIND
#define MER_VM_TRYLOCK(b, s)   vm_lock((b), (s))
#define MER_VM_REJECT()        vm_unlock_engine()
#define MER_VM_UNLOCK()        vm_unlock()
#else
#define MER_VM_TRYLOCK(b, s)   sceKernelVolatileMemTryLock(0, (b), (s))
#define MER_VM_REJECT()        sceKernelVolatileMemUnlock(0)
#define MER_VM_UNLOCK()        sceKernelVolatileMemUnlock(0)
#endif

/* `inproc_me_unload = 1`, the harness MUTANT of the in-process return
 * (g_inproc_me_unload and its comment, fe/inproc.b.inc.c); 0 in every other build. */
#ifdef GPSP_PERF_RIG
#define INPROC_ME_UNLOAD_MUTANT() (g_inproc_me_unload)
#else
#define INPROC_ME_UNLOAD_MUTANT() 0
#endif

/* The RFU trace ring is drained by rfu_trace_drain() (fe/link.d.inc.c) in a
 * telemetry build; without telemetry the call is nothing. */
#ifdef GPSP_NO_TELEMETRY
#define rfu_trace_drain() ((void)0)
#endif

/* fe_gblink.c NO_PEER_STEPS: the session's own wait for the partner's HELLO */
#define LINK_START_GRACE_US 30000000u

/* The rig's identity, audit and watch lines are functions of fe/harness.f.inc.c in
 * a telemetry build; a player build calls these no-ops instead.  (g_log_input,
 * the #define beside them there, is a macro-global: it stays.) */
#ifdef GPSP_KEEP_TELEMETRY
/* (this arm: no macro; fe/harness.f.inc.c defines the functions) */
#else
#define rig_preserve_log()          do { } while (0)
#define rig_boot_identity(h)        do { (void)(h); } while (0)
#define rig_rom_identity(p)         do { (void)(p); } while (0)
#define rig_cfg_audit_report()      do { } while (0)
#define rig_fail_probe()            do { } while (0)
#define rig_romload_evt(f)          do { (void)(f); } while (0)
#define rig_watch_evt(f)            do { (void)(f); } while (0)
#endif

/* The boot timeline (docs/UI-OVERLAY.md §12.2.1): g_boot_mark[], g_boot_t0,
 * boot_mark() and boot_timeline_emit() of fe/harness.f.inc.c in a telemetry build. */
#ifdef GPSP_KEEP_TELEMETRY
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
