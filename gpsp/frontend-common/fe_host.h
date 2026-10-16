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
    * clocks (Pokemon Gold/Silver/Crystal) on every boot.  For a GB/GBC game
    * this is UTC: it stamps the save's clock trailer, which every other
    * emulator reads and writes as UTC.  (A GBA link session also seeds its
    * cartridge clock from it; there it is the date the game shows.) */
   time_t (*wallclock)(void);
   /* GB/GBC saves written before the stamp was UTC (fe_host.c, "the
    * clock trailer's stamp is UTC").
    * Releases up to 3.5.0 stamped the trailer with wallclock_legacy -- on
    * the PSP, local time.  A trailer that does not carry the mark this
    * frontend now writes, stamped at or before gb_rtc_legacy_before on THAT
    * clock, is read on that clock: exactly the arithmetic those releases
    * did, so a player's clock does not jump by the UTC offset on the first
    * load after the upgrade.  Any other trailer is UTC.  NULL / 0 (every
    * host test, and any platform whose stamps were always UTC): all UTC. */
   time_t (*wallclock_legacy)(void);
   long long gb_rtc_legacy_before;
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
#ifdef GBA_LINK
   /* GBA link mirror, harness bench (docs/GBA-LINK-MIRROR.md): run a second
    * GBA core on the same cartridge with the cable between them, started
    * from every state the player's machine loads.  Its buttons are the
    * player's (gba_dual_mirror) or a frame-stamped script; lookahead in
    * cycles (0 = default); per-frame hashes of both machines to a file. */
   int gba_dual;
   int gba_dual_mirror;
   int gba_dual_own_rom;
   const char *gba_dual_script;
   const char *gba_dual_autopilot;
   unsigned gba_dual_lookahead;
   int gba_dual_partner_audio;   /* fe_gbadual_config.partner_audio */
   int gba_dual_sram_crc;        /* fe_gbadual_config.sram_crc */
   unsigned gba_dual_b_jit_pin_p1; /* fe_gbadual_config.b_jit_pin_p1 */
   int gba_dual_cable_fast;      /* fe_gbadual_config.cable_fast */
   int gba_dual_step_max_ms;     /* fe_gbadual_config.step_max_ms */
   /* fe_gblink_layout.core_mask: the guest-visible core switches this
    * console runs (bit 0 TIMER_LAZY); a session checks both ends agree */
   unsigned gba_core_mask;
   /* Bench only: start the pair at the first frame from the player's
    * power-on machine (no state file needed).  Without it, or a state load,
    * the bench runs ONE machine (GBA-LINK-MIRROR.md 21). */
   int gba_dual_cold;
   int gba_link_nolend;          /* 1: keep B's block out of the ROM cache */
   const char *gba_dual_hash;
   /* A two-console GBA link-cable session (fe_gblink over fe_gbadual's
    * machines, docs/GBA-LINK-MIRROR.md section 14): fe_host_gblink_start
    * works for a GBA game, and a state load no longer restarts the bench
    * pair.  Off unless the platform asks (harness gblink_role + gba_dual). */
   int gba_dual_session;
   /* Section 19: the session owns instance B's memory.  B is NOT loaded at
    * boot; its static block is lent to this game's ROM cache, and a session
    * start reloads this game's core without it (and loads B), a session end
    * the reverse.  gba_session_reserve_kb: heap held across the start's
    * reload for the session's own buffers (0 = 2560).  gba_reload_note:
    * the platform's screen during the reload -- phase 0 begin, 1 a step,
    * 2 done, -1 failed (what = the message for the player). */
   int gba_dual_late;
   /* Section 20: a cable session's translation-cache offsets mod 8 KiB, PLUS
    * ONE (0 = keep: A the single-player pin, B gba_dual_b_jit_pin_p1), per
    * tier ([0] SMALL = the PSP-1000, [1] LARGE = 3000/Go).  LAYOUT_PIN
    * builds; applied by the session start's reload, undone by the end's. */
   unsigned gba_sess_pin_a_p1[2], gba_sess_pin_b_p1[2];
   unsigned gba_session_reserve_kb;
   void (*gba_reload_note)(int phase, const char *what);
   /* Harness `sessmem_fail` (GBA-LINK-MIRROR.md 19.7): the session start
    * fails on purpose -- 1 the session's heap hold cannot be taken (asks
    * for 2 GiB), 2 the partner machine cannot be loaded -- so the
    * single-player restore and "Not enough memory for link play" are
    * exercised.  0 in every player's build. */
   int gba_sessmem_fail;
#endif
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
   /* GB link stall absorption (fe_gblink_platform.wait): while a step waits
    * for the partner's buttons, pump the network once and return nonzero
    * if this display frame still has time to finish; `waited_us` = this
    * step's wait so far.  NULL = a late input loses the display frame. */
   int (*gblink_wait)(uint32_t waited_us);
   /* GB link resend requests on the bulk lane (fe_gblink_platform.nack). */
   int gblink_nack;
   /* Harness A/B: slot-0 frames between sync hashes (0 = 240, GB and GBA
    * sessions alike), and the 3.1 beta's byte-wise GB hash
    * (fe_gblink_local.hash_legacy). */
   unsigned gblink_hash_every;
   int gblink_hash_legacy;

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
/* Restore a missing slot from an interrupted .old replacement. 0 = nothing
 * to recover, 1 = recovered, -1 = I/O/path failure. Call before UI slot stats. */
int fe_host_state_recover(const char *path);

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
/* Why the last boot refused to start because of the battery save: 0 it did
 * not, 1 the file exists but could not be read (it is left untouched), 2 no
 * memory for it.  A game is never run over a save it could not load: its
 * first flush would replace the player's file. */
int fe_host_battery_refusal(void);

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
/* Ask the writer thread for a save scan now (the PSP's suspend callback).
 * Safe from any thread: it only sets a flag and wakes the writer. */
void fe_host_sram_request_scan(void);

/* Harness only (psp sleep_sim_sav / sram_exit_retry; see fe_host.c).  The
 * first returns 1 if a .sav handle was open to kill. */
#ifdef GPSP_PERF_RIG
int fe_host_sram_test_kill_handle(void);
void fe_host_sram_test_exit_retry(int on);
#endif

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
/* After fe_host_shutdown, before the next fe_host_boot in the SAME process
 * (the PSP's in-process return to the game list): the core options back to
 * what the first boot found, so the next game starts as a fresh boot would. */
void fe_host_session_reset(void);

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
/* "Fast link" (two frames of buttons per packet, GBA Link Cable sessions):
 * fe_host_set_fast_link is this console's own setting, read when a session
 * starts -- it decides only when this console hosts.  fe_host_fast_link is
 * what the running session uses: 1 / 0, -1 with no GBA cable session;
 * *source (optional) = FE_GBLINK_FL_* (fe_gblink.h). */
void fe_host_set_fast_link(int on);
int  fe_host_fast_link(int *source);
void fe_host_gblink_status(char *line1, size_t cap1, char *line2, size_t cap2);
void fe_host_gblink_progress(int *p1, int *p2);   /* per mille, -1 = none */
/* The last step of a link session stalled (the frame shown is a repeat). */
int fe_host_gblink_stalled(void);
/* Telemetry: take (and reset) the running session's window
 * (fe_gblink_take_window); 0 when there is no session, else 1.  The step's
 * absorbed waiting is excluded from core_prof's `core`. */
struct fe_gblink_window;
int fe_host_gblink_window(struct fe_gblink_window *out);
void fe_host_gblink_request_end(void);
int  fe_host_gblink_close(void);
/* GBA-LINK-MIRROR.md 19 (harness): a session start's (1) or end's (0)
 * reload alone.  0 ok, -1 not enough memory (single-player restored), -2 not
 * in late mode or a session runs, -3 the game could not be reloaded. */
int  fe_host_gba_sessmem_cycle(int up);
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
/* The running session's facts for the LINKING screen (fe_gblink_progress);
 * 0 when there is no session (out is then FAILED/none), else 1. */
struct fe_gblink_progress;
int  fe_host_gblink_facts(struct fe_gblink_progress *out);
/* 1 when this boot can start a GBA link-cable session (GBA_LINK builds:
 * instance B loaded for sessions, docs/GBA-LINK-MIRROR.md 14). */
int  fe_host_gba_cable_ready(void);
/* GBA cable sessions identify the cartridge by sampled slices (default 1,
 * fe_gblink_local.rom_sampled); 0 = the streamed whole-file SHA-1.  Both
 * consoles must agree; harness A/B only. */
void fe_host_gba_cable_rom_sampled(int on);

#ifdef REWIND
/* ---- rewind (PROTOTYPE; docs/REWIND.md, fe_rewind.h) ----------------------
 * Compiled only with -DREWIND, and inert until fe_host_rewind_enable().
 * GBA games only; refused while any link session exists (lockstep and the
 * emulated wireless adapter cannot go back in time). */
typedef struct fe_host_rewind_opts
{
   /* The player's setting: megabytes of history, 1..8 (clamped); 0 = 2.
    * ring_bytes, when nonzero, overrides it exactly (tests: tiny rings). */
   uint32_t ring_mib;
   uint32_t ring_bytes;
   uint32_t interval;       /* frames between snapshots; 0 = 6 */
   uint32_t max_frames;     /* longest rewind in frames; 0 = 16384 */
   /* THE SAVE IS PERMANENT UNDER REWIND (the owner's rule, docs/REWIND.md
    * 17): a rewind never changes the cartridge save, in memory or in the
    * file, and a save write does not end the history.  There is no option.
    * save_quiet: N, the frames the save chip (and the link port) must have
    * been left alone before play may continue from a frame a rewind stops
    * on (0 = 60); a release inside moves to the nearest frame outside.
    * rlog_bytes: the log of save bytes the game read, served again to the
    * frames a rewind re-runs (0 = 131072; a power of two).
    * sav_mutant: TESTS ONLY.  1 = re-run frames read the live save, 2 =
    * re-run frames write the save, 4 = a rewind may stop anywhere. */
   uint32_t save_quiet;
   uint32_t rlog_bytes;
   int sav_mutant;
   /* 1 = frames run during fast-forward are recorded like any others;
    * 0 = fast-forward is a barrier. */
   int ff_rewindable;
   /* Test instruments.  hash_path: one line per frame run ("F") and per
    * frame shown backwards ("R"), see tools/rewind_check.py.  redo_inputs:
    * after a rewind, the frames that were gone back over are played again
    * with their LOGGED inputs instead of the platform's. */
   const char *hash_path;
   int redo_inputs;
   /* With hash_path: write the image hashed for timeline frame dump_frame,
    * every time that frame is run, to dump_dir/state_<n>_<k>.bin (0 = the
    * RW_DUMP_FRAME / RW_DUMP_DIR environment, where there is one). */
   uint32_t dump_frame;
   const char *dump_dir;
   /* 0 (default) = a restore keeps the ROM translation cache and flushes
    * only the RAM one; 1 = what a state load does, both.  The default is
    * the cheaper one AND the exact one: emptying the ROM cache changes where
    * its blocks begin, and with that the instruction a frame ends on
    * (docs/REWIND.md section 6). */
   int full_flush_restore;
   /* Emulated frames a rewind tick may run at most (fe_rewind_config.burst;
    * 0 = 2), and the time after which it stops running more
    * (fe_rewind_config.tick_budget_us; 0 = no limit). */
   uint32_t burst;
   uint32_t tick_budget_us;
   /* Test instrument: from timeline frame fuzz_from on (0 = off) the buttons
    * are a fixed pseudo-random function of the frame number and fuzz_seed,
    * whatever the platform or a script says. */
   uint32_t fuzz_from, fuzz_seed;
   /* 1 = keep no pictures or sound and present nothing while rewinding:
    * the picture simply holds.  For a platform whose picture does not come
    * out of the core's video callback (the PSP's second-core renderer) until
    * its own capture path exists; everything else -- snapshots, restores,
    * re-runs, the timeline -- is the real thing, so it can be measured. */
   int no_capture;
   /* How a snapshot reads the state (fe_host.c, "how the state reaches a
    * snapshot").  inplace: the big arrays are compared in the machine's own
    * memory, nothing is copied; 0 = the whole image is serialised first.
    * dirty (with inplace): pages nothing wrote to since the last snapshot
    * are not read at all, by the core's written-page map (gba_memory.h).
    * Bit 0 = VRAM, bit 1 = EWRAM; 3 = both. */
   int inplace;
   int dirty;
   /* Test instrument: after every snapshot compare the whole machine with
    * the reference (fe_rewind_config.verify).  A difference is a page the
    * map hid: EVT rewind_verify_fail, and anomalies at exit is nonzero. */
   int verify;
   /* Measurement: time the pieces of the serialised (inplace = 0) gather
    * separately; EVT rewind_gprof on the heartbeat cadence. */
   int gather_prof;
   /* Wall-clock pacing of snapshots (fe_rewind_config.pace_us): 0 = one
    * every `interval` frames however fast they run. */
   uint32_t pace_us;
   /* NO SPEED REGRESSION: a snapshot is not taken on a frame that has no
    * time for it.  slack_margin_us != 0 turns it on: after a normal-speed
    * frame whose core time + this margin (what the platform itself needs
    * per frame: presenting, sound) + the cost of the last snapshots would
    * pass the frame period, the snapshot waits for a frame with room, at
    * most max_span frames (0 = 8 * interval).  A game with no room at all
    * then pays for one snapshot every max_span frames instead of every
    * interval. */
   uint32_t slack_margin_us;
   uint32_t max_span;
   /* 1 = a restore copies back only the pages that can differ from the
    * snapshot (the written-page map plus what the deltas stepped over
    * changed), keeps translated RAM code when no byte under it changed and
    * clears only the stretch of the sound ring in flight.  0 = everything,
    * as stage 2 did.  Needs inplace; the page part needs dirty. */
   int fast_restore;
   /* 1 = while recording the cartridge clock is base + frames, brought up
    * to the wall clock when a snapshot is taken (gba_memory.c,
    * gba_rewind_clock_frames): a re-run frame reads the second it read the
    * first time.  0 = the wall clock at every read, as without rewind. */
   int clock_frames;
   /* THE PICTURES.  A captured picture is pix_rows rows of pix_pitch
    * pixels, of which 240 x 160 are the game's (0, 0 = 240 x 160).
    * pix_begin, when set, is called before EVERY frame the rewind runs or
    * records with the slot that frame's picture belongs in (NULL: this
    * frame keeps none).  It returns 1 if the platform's own renderer will
    * draw into the slot -- then pix_end() after the frame says whether it
    * did -- or 0 for "copy the core's picture there yourself".
    * show, when set, replaces the usual video sink while rewinding: present
    * this slot (NULL = this frame has no picture, keep what is up). */
   uint32_t pix_pitch, pix_rows;
   int  (*pix_begin)(uint16_t *slot);
   int  (*pix_end)(void);
   void (*show)(const uint16_t *slot);
   /* THE SOUND while rewinding.  0 = the frames gone back over are heard
    * backwards; 1 = silence (their samples are not kept either); 2 =
    * silence, and the frames a held rewind re-runs skip this emulator's own
    * sound synthesis (the game's sound code on the emulated CPU runs as
    * ever).  Those frames are then not exact copies of the first pass in
    * the sound buffer, so the machine is restored once more when the
    * control is let go (fe_rewind_taint). */
   int sound_mode;
   /* Seconds of history to aim for (fe_rewind_set_target; 0 = snapshots at
    * a fixed distance), and the most frames between two snapshots it may
    * stretch to. */
   uint32_t target_s, gap_max;
} fe_host_rewind_opts;

typedef struct fe_host_rewind_info
{
   int      active;         /* a rewind is in progress */
   uint32_t back_frames;    /* how far behind its starting point it is */
   uint32_t avail_frames;   /* how much further back it can go */
   uint32_t ring_used, ring_bytes, records;
   /* The control is held and there is nothing earlier to show, and why
    * (FE_RW_LIMIT_*): what the readout says at the stop. */
   int      at_limit;
   int      limit_reason;
   uint32_t now_frame, mach_frame;   /* timeline: on screen / the machine */
   uint32_t stepped;        /* frames the last tick went back */
   /* What things have been costing, microseconds (0 = not seen yet): a
    * normal frame, a frame re-run with / without a picture, a restore
    * (unpacking included).  For a platform that picks how often to show a
    * picture from what the game can sustain. */
   uint32_t frame_us, rerun_pic_us, rerun_head_us, restore_us;
   uint32_t ring_granted;   /* bytes of ring actually obtained */
} fe_host_rewind_info;

/* Why the history starts where it does. */
#define FE_RW_LIMIT_HISTORY 0   /* the buffer: this is as far back as it holds
                                 * (or recording began here) */
#define FE_RW_LIMIT_SAVE    1   /* the game has read more of its save since
                                 * than the read log holds */
#define FE_RW_LIMIT_STATE   2   /* a state was loaded here */
#define FE_RW_LIMIT_CHEAT   3   /* the cheat list changed here */
#define FE_RW_LIMIT_OTHER   4   /* anything else */
#define FE_RW_LIMIT_SERIAL  5   /* the game used the link port here */
#define FE_RW_LIMIT_SLEEP   6   /* the console slept here */
#define FE_RW_LIMIT_POKE    7   /* the frontend wrote the game's memory */
#define FE_RW_LIMIT_OFF     8   /* not recorded: a link session, or
                                 * fast-forward when that is not kept */
#define FE_RW_LIMIT_N       9

/* Optional, before reserve/enable: a block of memory the platform already
 * holds that the rewind may live in instead of the heap (the PSP's volatile
 * pool).  It is used from the END downwards; fe_host_rewind_pool_low() says
 * how far down it reached, so the owner of the other end can check the two
 * do not meet.  If everything does not fit, the delta ring alone goes to the
 * heap; if not even the rest fits, the pool is not used.
 *
 * fe_host_rewind_suspend: the pool is about to be taken away (PSP sleep).
 * Recording stops, the history is gone, nothing points into the pool any
 * more; a heap ring is kept.  fe_host_rewind_resume(base, bytes) starts
 * again with the same options in the pool as it is now (NULL, 0 = none:
 * heap).  Both are safe to call when rewind is off. */
void  fe_host_rewind_set_pool(void *base, size_t bytes);
void *fe_host_rewind_pool_low(void);
void  fe_host_rewind_suspend(void);
int   fe_host_rewind_resume(void *base, size_t bytes);
/* ENDING THE HISTORY IN THE MIDDLE OF A REWIND (docs/REWIND.md 22).  While a
 * rewind is in progress the machine is not at the frame on screen: it is
 * re-running old frames, perhaps in the middle of an old save sequence, with
 * the save write-protected.  Play may only go on from a frame the no-land
 * rule has vetted, so whoever is about to take the history away (a sleep),
 * or to do anything else to the machine (a state save, the menu), calls
 *   fe_host_rewind_land(budget_us)
 * first, on the emulation thread: it lets the control go and runs the
 * rewind's own ticks until play may resume (1) or the budget is spent (0;
 * budget 0 = as long as it takes).  fe_host_rewind_busy() says whether there
 * is anything to land.
 * A history that is nevertheless ended unlanded (fe_host_rewind_suspend
 * after a 0) continues from where the machine stands when that frame passes
 * the same rule, and otherwise the game is RESET before its next frame (a
 * cartridge switched off and on: it starts from the save, which no re-run
 * frame ever touched).  fe_host_rewind_abandon(): the history was suspended
 * and will not be resumed (the platform gave the rewind up). */
int   fe_host_rewind_busy(void);
/* 1 = right now the machine stands on a frame play may not continue from
 * (inside an old save sequence that a later save has overtaken): the moment
 * a platform should not let the history be taken away at all, if it can
 * help it (the PSP holds the power switch off, psp_rewind_pwr). */
int   fe_host_rewind_unvetted(void);
int   fe_host_rewind_land(uint32_t budget_us);
/* Before a fe_host_rewind_suspend: the history's own memory cannot be
 * believed (the pool was kept across a sleep that tore nothing down), so a
 * rewind in progress cannot be vetted from it: it counts as unvetted. */
void  fe_host_rewind_distrust(void);
void  fe_host_rewind_abandon(void);
/* How often that last resort was taken (the platform tells the player). */
unsigned fe_host_rewind_resets(void);

/* Before fe_host_boot, with the options the game will record with (NULL =
 * it will not): tells the core whether its store paths need to mark written
 * pages at all.  A game started without it records with every page read at
 * every snapshot; a game started with NULL pays nothing for the feature.
 * fe_host_rewind_reserve does this too. */
void fe_host_rewind_prepare(const fe_host_rewind_opts *o);

/* Reserve the memory (one malloc, here and never again) and start recording.
 * Call after fe_host_boot.  0 = ok.
 * fe_host_rewind_reserve, optional, takes that memory EARLIER with the same
 * options -- before fe_host_boot, on a platform whose core then fills the
 * heap with the cartridge (the PSP-1000).  0 = ok; enable then uses it. */
int  fe_host_rewind_reserve(const fe_host_rewind_opts *o);
int  fe_host_rewind_enable(const fe_host_rewind_opts *o);
/* NULL when rewind can be used now, else why not ("off", "gb", "link"). */
const char *fe_host_rewind_blocked(void);
/* Tell the recorder whether the platform is fast-forwarding. */
void fe_host_rewind_set_ff(int ff);
/* Call once per display tick, BEFORE deciding to run a frame, with the
 * level of the rewind control (1 = down).  There is no begin or end: any
 * pattern of presses is fine (fe_rewind.h).
 *   2 (FE_RW_PLAY)  not rewinding: call fe_host_run_frame() as usual
 *   1 (FE_RW_SHOW)  an earlier frame was presented through the usual sinks
 *   0 (FE_RW_HOLD)  the picture holds this tick (start of the history, or
 *                   the machine is on its way to the frame on screen)
 * Do not run a frame on 1 or 0. */
int  fe_host_rewind_tick(int held);
/* The same, going back up to `step` frames in the tick (fe_rewind.h);
 * fe_host_rewind_info.stepped says how many it managed.  step == 0 while
 * held: a tick between two pictures (the work goes on, nothing is shown). */
int  fe_host_rewind_tick_step(int held, uint32_t step);
/* The cadence the platform shows at: one picture every `stride` frames
 * (fe_rewind_set_stride).  quiet: the sound of the frames gone over is not
 * played (it is, played backwards, only at one picture a frame a tick). */
void fe_host_rewind_set_cadence(uint32_t stride, int quiet);
/* What the next ticks may spend re-running frames (fe_rewind_set_budget). */
void fe_host_rewind_set_tick_budget(uint32_t us);
/* 1 (default): a frame re-run without a picture is run as a skipped frame,
 * so the core does not capture it for a renderer either. */
void fe_host_rewind_set_head_skip(int on);
/* 1 while the core is re-running an old frame for the rewind (inside
 * fe_host_rewind_tick).  For platform hooks the core calls mid-frame: such a
 * frame must not be presented, posted to a renderer or counted. */
int  fe_host_rewind_replaying(void);
void fe_host_rewind_info_get(fe_host_rewind_info *out);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FE_HOST_H */
