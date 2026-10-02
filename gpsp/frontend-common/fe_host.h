/* fe_host.h — libretro host glue shared by the SDL desktop twin and the PSP
 * frontend (plan §4.2). Statically linked against the gpsp libretro core;
 * answers the environment calls enumerated in docs/FRONTEND-AUDIT.md §1,
 * plumbs video/audio/input, and owns the SRAM load/flush discipline
 * (FRONTEND-AUDIT §5: no core dirty signal — CRC hash-compare + flush on
 * exit).  All EVT logging for boot/rom/bios/sram lives here so both
 * frontends emit identical grep-stable markers.
 */
#ifndef FE_HOST_H
#define FE_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GBA timing truths (FRONTEND-AUDIT §2/§3). */
#define FE_GBA_FPS      (16777216.0 / 280896.0)  /* 59.7275 Hz */
#define FE_GBA_WIDTH    240
#define FE_GBA_HEIGHT   160
#define FE_SRAM_SIZE    0x20000                  /* core always reports 128 KiB */

typedef struct fe_host_config
{
   /* Select the execution backend. Zero/default preserves the GBA libretro
    * core for existing callers. */
   fe_console_t console;
   const char *rom_path;     /* full path; core opens it itself (need_fullpath) */
   const char *system_dir;   /* directory containing gba_bios.bin */
   const char *save_path;    /* GBA: .sav (full 128 KiB image); GB/GBC:
                              * battery RAM plus any cartridge clock */

   /* Platform sinks. video data==NULL means "duped frame — re-present the
    * previous one" (core frameskip contract, FRONTEND-AUDIT §2). */
   void (*video_frame)(const uint16_t *rgb565, unsigned w, unsigned h,
                       size_t pitch_bytes);
   void (*audio_frames)(const int16_t *stereo_lr, size_t frames);
   /* Current pad state as RETRO_DEVICE_ID_JOYPAD_* bitmask. */
   uint32_t (*input_bitmask)(void);
   /* Host wall-clock in microseconds, monotonic (PSP:
    * sceKernelGetSystemTimeWide; SDL: CLOCK_MONOTONIC — same sources the
    * netdrv timers use). Optional; heartbeat logs t_us=0 when absent.
    * Logging (hw perf baseline: fps = d(frames)/d(t_us)) and the GB cable's
    * byte timeout; without it a GB link byte gives up after one pump. */
   uint64_t (*time_us)(void);
   /* Optional yield while a GB byte-cable transfer waits for its peer. */
   void (*yield_thread)(void);
   /* Optional wall clock (seconds since 1970) for GB/GBC cartridge RTCs.
    * NULL = C time(), which on a PSP is roughly uptime and resets MBC3
    * clocks (Pokemon Gold/Silver/Crystal) on every boot. */
   time_t (*wallclock)(void);
   /* GB/GBC only: the DMG palette, a GBCORE_PALETTE_* id (0 = Auto: Super
    * Game Boy colours where the game has them, grey otherwise). */
   int gb_palette;
   /* GBC only: 1 = show CGB colours raw (the 5-to-8-bit expansion); 0, the
    * default, = through the Game Boy Color LCD model
    * (gbcore_set_color_correction; docs/GB-PALETTE-FIXES.md). */
   int gbc_raw_colors;
   /* GB/GBC, harness measurement only (GPSP_PERF_RIG's `gb_dual_rom`): also
    * run this second cartridge headless beside the player's, interleaved
    * one scanline at a time with the cable wired -- the machine pair of a
    * GB link session (gbcore_dual.h), without the network.  NULL = off.
    * gb_dual_mirror: the second machine gets the player's buttons (1) or
    * none (0). */
   const char *gb_dual_rom;
   int gb_dual_mirror;
   unsigned gb_dual_batch;   /* bench: scanlines per turn (gbdual_config) */
   int gb_bench_headless;    /* bench: the single machine runs headless */
   /* ...and optionally starts it from this save state (a .stN file). */
   const char *gb_dual_state;
   /* GB link sessions: the platform's library lookup for the partner's
    * cartridge -- a file of exactly `size` bytes whose header (0x134-0x14F)
    * matches; the session checks its SHA-1.  NULL = never in the library
    * (the partner then sends it). */
   int (*gb_find_rom)(uint32_t size, const uint8_t header[0x1C], char *path,
                      size_t cap);
   /* Harness: the autopilot script belongs to a link session, so it waits
    * (fe_host_autopilot_gate) until the session's machines are running. */
   int gblink_script_waits;
   /* GB link sessions hosted here: scanlines slot 0 may run ahead while
    * the cable is quiet (gbdual_config_t.batch_lines; 0/1 = strict). */
   unsigned gblink_batch;
   /* Link sessions start from the running games' save states (the partner
    * loads yours) and each game continues from its own state afterwards --
    * no restart either side.  0 = power on from the battery images. */
   int gblink_live;
   /* Frames of this player's input repeated in each unreliable input
    * datagram (fe_gblink_platform.input_copies; 0 = off). */
   unsigned gblink_input_copies;
   /* Harness: after a session, boot the game from its battery save (as a
    * power cycle would) instead of continuing -- the clock check. */
   int gblink_resume_boot;
   int gblink_pace_slot0;      /* fe_gblink_local.pace_slot0 */
   /* Frames between the periodic telemetry lines (heartbeat, fps,
    * core_prof, gblink_stats); 0 = HEARTBEAT_INTERVAL (600). */
   unsigned heartbeat_frames;
   /* Harness A/B only: 1 = write the session's clock stamp back unchanged
    * (the behaviour before the fix), to show the clock prompt it caused. */
   int gblink_rtc_keep_session_stamp;
   /* GB link bulk lane (optional; fe_gblink.h): large unreliable datagrams
    * to the partner's radio address, and the ones received.  enable(1) at a
    * session's start, enable(0) at its end.  gb_bulk_max = datagram bytes
    * (0 = the ordered channel carries transfers); gb_bulk_rate = datagrams
    * a frame (0 = adaptive). */
   int (*gb_bulk_enable)(int on);
   int (*gb_bulk_send)(const uint8_t mac[6], const void *buf, size_t len);
   int (*gb_bulk_recv)(void *buf, size_t cap);
   size_t gb_bulk_max;
   unsigned gb_bulk_rate;

   /* Optional (ADR-0028/0029): read the core's monotonic perf counters — ROM
    * translation-cache flushes, RAM flushes, 32 KiB ROM page faults. Lets
    * fe_host record WHAT the core was doing on its slowest frame instead of
    * leaving the spike unattributed. Supplied by the frontend so this file
    * stays core-agnostic; NULL = no attribution, zero overhead.
    * ADR-0029 splits the RAM flush by CAUSE: `ram_full` = the RAM JIT cache
    * ran out of room (a bigger cache helps); `ram_smc` = a CPU store hit RAM
    * holding translated code; `ram_dma` = a DMA did. For the latter two a
    * bigger cache is useless — only finer-grained invalidation helps, and
    * only the DMA case knows its destination range up front. Counted
    * separately, not nested. */
   void (*core_counters)(unsigned *rom_flush, unsigned *ram_full,
                         unsigned *ram_smc, unsigned *ram_dma,
                         unsigned *page_load);

   /* Optional (ADR-0030): take and RESET the core's SMC write-address profile
    * -- where the SMC-triggering writes landed since the last call, bucketed
    * by 256-byte page.  `core_counters` says how often the game writes over
    * translated code; this says whether it is a few hot pages (real overlay
    * swapping) or a wide scatter (data writes hitting over-tagged code).
    * `hits` = total events, `pages` = distinct pages touched, `iw`/`ew` = the
    * IWRAM/EWRAM split, `oth` = events whose region could not be attributed
    * (must be 0; nonzero invalidates the split).  `hot_addr`/`hot_cnt` receive
    * the `nhot` hottest pages, descending, as GBA addresses; `top_addr`/
    * `top_cnt` the `nhot` hottest EXACT addresses (approximate counts from a
    * small heavy-hitter table, `ovf` = how many events had to evict a slot).
    * Supplied by the frontend so this file stays core-agnostic; NULL = no
    * line, zero cost. */
   void (*smc_addr)(unsigned *hits, unsigned *pages, unsigned *iw,
                    unsigned *ew, unsigned *oth, unsigned *ovf,
                    unsigned *hot_addr, unsigned *hot_cnt,
                    unsigned *top_addr, unsigned *top_cnt, unsigned nhot);

   /* Optional (phase 5g): take and RESET the core's SMC BLOCK profile -- which
    * translated block covers the watched address, where the scanner decided
    * that block ended and why, which GBA PC keeps writing there, and a
    * one-shot snapshot of the instruction words involved.  `smc_addr` says
    * WHICH address the storm lands on; this says whether that address is code
    * the game executes or data a block over-extended across.  Array arguments
    * are sized by the core's SMC_BLK_SLOTS / SMC_WR_SLOTS / SMC_BLK_SNAP; the
    * frontend passes buffers of FE_SMC_BLK_MAX and the callee fills what it
    * has.  NULL = no line, zero cost. */
   void (*smc_block)(unsigned *watch, unsigned *xlat, unsigned *ovf,
                     unsigned *start, unsigned *end, unsigned *mode,
                     unsigned *reason, unsigned *cnt,
                     unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                     unsigned *wovf,
                     unsigned *head, unsigned *ctx, unsigned *snap,
                     unsigned *fus, unsigned *fbytes, unsigned *fwide,
                     unsigned *same, unsigned *diff, unsigned *unkn);

   /* Optional (phase 5h): take and RESET the core's per-PHASE frame bracket.
    * ADR-0031 eliminated the SMC flush storm as the cause of the field's
    * 21.3 ms `core` max, which left the spike unattributed; this splits
    * retro_run into CPU execution / per-scanline video / the end-of-frame
    * blit / audio mixing and output / dynarec compilation / emulated RFU,
    * with the residues named rather than hidden.  `mean`/`max`/`worst` are
    * arrays of FE_CPH_NPHASE microsecond values indexed by FE_CPH_*; `max`
    * is each phase's own worst frame (so the maxima need NOT sum to the
    * frame maximum) and `worst` is the whole breakdown of the single frame
    * with the largest total, which is the one that attributes.  NULL = no
    * line, zero cost. */
   void (*core_phase)(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                      unsigned *reads, unsigned *mean, unsigned *max,
                      unsigned *worst, unsigned nph,
                      unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                      unsigned *wbkw, unsigned *ugba, unsigned *dman,
                      unsigned *stmr, unsigned *negc, unsigned *nego,
                      /* ADR-0051: rfu_transfer() calls and us per frame, plus
                       * the same two on the worst-`tot` frame. These are work
                       * charged to `cpu`, not to the `rfu` phase. */
                      unsigned *rfux, unsigned *rfut,
                      unsigned *wrfux, unsigned *wrfut);
   /* Optional, synchronous boot-only status. Called on the booting thread;
    * must not re-enter the core. NULL keeps desktop/other hosts unchanged. */
   void (*boot_status)(const char *stage);
} fe_host_config;

/* Must be >= the core's SMC_BLK_SLOTS / SMC_WR_SLOTS / SMC_BLK_SNAP. */
#define FE_SMC_BLK_SLOTS 4
#define FE_SMC_WR_SLOTS  4
#define FE_SMC_BLK_SNAP  16

/* Phase slots, must match the core's CPH_* (main.h). */
#define FE_CPH_TOT     0
#define FE_CPH_CPU     1
#define FE_CPH_VID     2
#define FE_CPH_BLT     3
#define FE_CPH_AMIX    4
#define FE_CPH_AOUT    5
#define FE_CPH_DSND    6
#define FE_CPH_JIT     7
#define FE_CPH_RFU     8
#define FE_CPH_OTH     9
#define FE_CPH_NPHASE 10

/* Override a core option before fe_host_boot (defaults per FRONTEND-AUDIT §9:
 * gpsp_bios=official, gpsp_serial=auto, gpsp_frameskip=disabled, ...).
 * Returns 0 on success, -1 if the key is unknown. */
int fe_host_option_set(const char *key, const char *value);

/* Change a runtime-changeable core option after boot (e.g. gpsp_frameskip
 * for fast-forward, plan §4.4): updates the table and arms
 * GET_VARIABLE_UPDATE so the core re-reads options on its next frame.
 * Do NOT use for load-only keys (bios/serial/...). Returns 0 on success. */
int fe_host_option_set_live(const char *key, const char *value);

/* Full boot: retro_set_environment/init/load_game + SRAM restore.
 * Emits: EVT bios=..., EVT rom_loaded code=XXXX size=N, EVT av_info ...,
 * EVT sram_load ... . Returns 0 on success. */
int fe_host_boot(const fe_host_config *cfg);

/* One emulated frame: retro_run + periodic SRAM dirty check + heartbeat. */
void fe_host_run_frame(void);
/* Force the NEXT fe_host_run_frame's video to be drawn (0) or skipped (1),
 * overriding the frameskip option; -1 hands the decision back to it.  A
 * skipped frame is emulated in full but neither rendered nor captured -- the
 * same contract as the core's own frameskip (video callback with NULL).
 * Used by the fast-forward draw gate (docs/FF-PRESETS.md); GBA and GB. */
void fe_host_skip_override(int skip);
/* Emulation faults the core recovered from by resetting the game (a guest
 * jump to an address that is not code).  Monotonic; the UI toasts on change. */
unsigned fe_host_guest_faults(void);

/* Report the platform audio buffer's state to the core, once per frame
 * (ADR-0018). Only meaningful with gpsp_frameskip=auto/auto_threshold, which
 * skip a VIDEO frame when `underrun_likely` — i.e. when the platform is not
 * holding real time. No-op unless the core asked for the callback.
 *   active          : 1 while audio is actually being consumed (0 when muted,
 *                     e.g. during fast-forward — the core then never skips).
 *   occupancy_pct   : buffer fill, 0-100.
 *   underrun_likely : 1 if the buffer is about to run dry. */
void fe_host_audio_buffer_status(int active, int occupancy_pct,
                                 int underrun_likely);

/* Frames emulated since boot. */
unsigned fe_host_frame_count(void);

/* ADR-0027 §measurement: cumulative cost of retro_run() itself — the one
 * per-frame cost ADR-0021 never bracketed.  `total_us` is cumulative since
 * boot (wraps at ~71 min of core time; callers take deltas), `max_us` is the
 * largest single call and is never reset. */
void fe_host_core_prof(unsigned *calls, uint32_t *total_us, uint32_t *max_us);

/* Audio sample rate the core actually reports (ADR-0028), valid after
 * fe_host_boot; 0 before. **Platforms must resample from this, never from a
 * literal** — gpsp_sound_rate is a core option and changing it must not
 * silently desync every frontend's resampler. */
unsigned fe_host_sample_rate(void);

/* ADR-0028: what the core was doing on its single slowest retro_run so far —
 * the per-frame counter deltas captured on the frame that set `max_us`.
 * All zero means the spike was none of these. Requires cfg.core_counters.
 * NOTE the all-time max is usually set during boot, so this snapshot latches
 * early; for "what is happening now" read `EVT core_prof`'s `wspike=`, which
 * is re-based every heartbeat. */
void fe_host_core_spike(uint32_t *max_us, unsigned *rom_flush,
                        unsigned *ram_full, unsigned *ram_smc,
                        unsigned *ram_dma, unsigned *page_load);

/* Emulated / rendered / skipped frame totals since boot (ADR-0019).
 * `rendered` counts video_refresh calls carrying real pixels; `skipped`
 * counts the core's NULL-data (frameskipped) presents.  Any pointer may be
 * NULL.  Also emitted as `EVT fps` on the heartbeat cadence. */
void fe_host_frame_stats(unsigned *emulated, unsigned *rendered,
                         unsigned *skipped);

/* Last presented frame (for BMP dumps); NULL if none yet. */
const uint16_t *fe_host_last_frame(size_t *pitch_bytes);

/* Savestate to/from a file.  GBA: 416 KiB fixed size (FRONTEND-AUDIT §7).
 * GB/GBC: the TGB Dual image (gbcore_state_save), about 20-190 KiB with the
 * cartridge, refused unless it was made for the same cartridge and console.
 * Emits EVT state_save/state_load. Returns 0 on success.  Callers must
 * enforce the session block themselves (plan §4.5: savestates are blocked
 * while a wireless session is active — RFU state is not serialized). */
int fe_host_state_save(const char *path);
int fe_host_state_load(const char *path);

/* GB/GBC DMG palette (GBCORE_PALETTE_* ids).  Setting it applies to the
 * running game when there is one, and to every later boot; the names are
 * for menus. */
void        fe_host_gb_palette_set(int id);
int         fe_host_gb_palette_count(void);
const char *fe_host_gb_palette_name(int id);

/* CRC-compare the 128 KiB SRAM buffer and write the .sav if dirty.
 * force_write=1 writes even when clean (used by round-trip tests) AND
 * drains to completion before returning — use it for exit/menu flushes.
 * A periodic (force_write=0) call may leave writes in flight for
 * fe_host_sram_service() to finish over the following frames (ADR-0020).
 * Returns 1 if a write was started/done, 0 if clean, -1 on I/O error. */
int fe_host_sram_flush(int force_write);

/* Advance an in-flight .sav write by a few 4 KiB blocks, bounded by a
 * wall-clock budget (ADR-0020).  Called automatically from
 * fe_host_run_frame; a frontend that pumps the core by other means should
 * call it once per frame.  No-op when nothing is pending.
 * Exists so the emulation thread is never stalled for hundreds of ms by a
 * save write — on real hardware that starves the wireless link badly enough
 * for the game to declare a fatal RFU error. */
void fe_host_sram_service(void);

/* Finish any in-flight .sav write, however long it takes.  Call at every
 * point that must not lose bytes (fe_host_shutdown already does; add it to
 * any suspend/savestate path you introduce).  Budgeted draining is a
 * smoothness optimisation and must never become a durability regression. */
void fe_host_sram_sync(void);

/* ---- asynchronous .sav writer (ADR-0025) --------------------------------
 * ADR-0020's budgeted drain bounded the stall but did not remove it: the
 * field still measured `EVT sram_flush ... ms=29.296` for one 14-block delta
 * write, in the very heartbeat window whose `frame` maximum jumped from
 * 37264 to 63772 us.  A 4 KiB write to a memory stick is simply not a
 * bounded operation, so the only fix is to stop doing it on the emulation
 * thread at all.
 *
 * Install an io interface and fe_host stops touching the .sav: the dirty
 * SCAN stays on the emulation thread (it is CPU, not I/O), but every open,
 * seek, write, flush and close happens on the caller's writer thread inside
 * fe_host_sram_service_io().  NULL (the desktop default) keeps ADR-0020's
 * synchronous budgeted drain exactly as it was.
 *
 * ORDERING CONTRACT — there is no lock, because there never needs to be one:
 *   1. Install the interface only after fe_host_boot().
 *   2. Exactly ONE thread calls fe_host_sram_service_io().
 *   3. Stop that thread and call fe_host_set_io(NULL) BEFORE
 *      fe_host_shutdown(), so the final flush/sync/close — and
 *      retro_unload_game(), which frees the buffer the writer reads — run
 *      single-threaded.
 * The emulation thread only ever RAISES a block's dirty flag and never
 * clears one, so a block the game touches mid-write is simply seen as dirty
 * again; the writer CRCs the staging copy it actually wrote, so the recorded
 * CRC always matches the bytes on disk. */
typedef struct fe_host_io
{
   void (*wake)(void);   /* signal the writer thread; cheap, may be frequent */
   void (*yield)(void);  /* sleep ~1 ms — the writer runs BELOW the caller,
                          * so a sync wait must actually yield to progress */
} fe_host_io;

void fe_host_set_io(const fe_host_io *io);

/* Called ONLY by the writer thread.  Drains every pending block (no budget:
 * nothing here is on the frame path).  Returns blocks written. */
int fe_host_sram_service_io(void);

/* FF durability nudge (see fe_host.c).  Call once per main-loop iteration on
 * the EMULATION thread, after the core has run, with ff_active = "fast-forward
 * is engaged".  Does nothing unless a writer thread is installed, a GBA game
 * has written backup memory, and the writer has not yet persisted it; then,
 * only while ff_active and once the save has settled, it requests a scan and
 * yields ~1 ms through the io interface, at most every few frames, until the
 * writer confirms.  Returns 1 if it yielded this call. */
int  fe_host_sram_ff_nudge(int ff_active);
/* Harness A/B: 0 disables the yield (detection and logging stay on). */
void fe_host_sram_ff_nudge_enable(int on);

/* CRC32 of the live 128 KiB SRAM buffer right now (autopilot save-detect). */
uint32_t fe_host_sram_crc_now(void);

/* Read emulated RAM by GBA address (autopilot predicates, FRONTEND-AUDIT §6):
 * resolved through the SET_MEMORY_MAPS descriptors (IWRAM 0x03000000/0x8000 +
 * EWRAM 0x02000000/0x40000), with retro_get_memory_data(SYSTEM_RAM) as the
 * EWRAM fallback. Returns 0 on success, -1 if unmapped. */
int fe_host_mem_read(uint32_t gba_addr, void *out, unsigned len);

/* ADR-0079: the write twin.  Same descriptor mapping and bounds rules as
 * the read; returns 0 only if every byte landed.  Exists for exactly one
 * caller — the exit-echo state assist — and any new caller should be
 * treated with the suspicion a guest-RAM write deserves. */
int fe_host_mem_write(uint32_t gba_addr, const void *in, unsigned len);

/* OR this mask into the pad state the core polls (autopilot input).
 * Persists until the next call; pass 0 to release. */
/* ADR-0046: the mask input_state() actually hands the core (pad | injected).
 * Use this for telemetry -- reading the platform pad alone makes the probe
 * invisible to the harness, which injects. */
uint32_t fe_host_input_mask(void);
void fe_host_input_inject(uint32_t joypad_mask);

/* Netpacket interface the core registered via env call 78 during boot,
 * or NULL. Points at a struct retro_netpacket_callback (returned as void*
 * so this header stays libretro.h-free); consumed by netpacket_host. */
const void *fe_host_netpacket_cb(void);

/* Flush SRAM (dirty-check), retro_unload_game + retro_deinit. */
void fe_host_shutdown(void);

/* ---- GB link sessions (fe_gblink.h) -------------------------------------
 * After fe_np_start_gb has brought the ad-hoc session up: flush and stop the
 * running game and start the session (host/guest; input delay in frames;
 * rtc_seed 0 = this console's clock).  fe_host_run_frame then steps it.
 * fe_host_gblink_state: -1 none, else FE_GBLINK_* (SETUP, RUNNING, ENDING,
 * DONE, FAILED).  Once DONE or FAILED, fe_host_gblink_close boots the game
 * again from its (possibly just committed) save. */
int  fe_host_gblink_start(int is_host, unsigned input_delay, int64_t rtc_seed);
int  fe_host_gblink_active(void);
int  fe_host_gblink_state(void);
void fe_host_gblink_status(char *line1, size_t cap1, char *line2, size_t cap2);
void fe_host_gblink_progress(int *p1, int *p2);   /* per mille, -1 = none */
/* The last step of a link session stalled (the frame shown is a repeat). */
int fe_host_gblink_stalled(void);
void fe_host_gblink_request_end(void);
int  fe_host_gblink_close(void);
/* Harness tuning: cartridge/save chunks sent per frame (0 = default). */
void fe_host_gblink_tune(unsigned bulk_per_frame);
/* 1 once nothing this console sent the partner is still unacknowledged (or
 * the partner is gone): only then may the radio go down after a session.
 * Tearing it down at DONE lost the last FINAL in hardware run hw1 (arm W):
 * the partner never saw it and ended FAILED, its save unwritten. */
int  fe_host_gblink_flushed(void);
/* Harness: stop running the single machine (the game waits, as behind the
 * pause menu) until a session starts, so its state at link time does not
 * depend on how long the radio took. */
void fe_host_gblink_hold(int on);
/* 1 when the autopilot may take its per-frame step now (always, outside a
 * link session unless gblink_script_waits). */
int  fe_host_autopilot_gate(void);

#ifdef __cplusplus
}
#endif

#endif /* FE_HOST_H */
