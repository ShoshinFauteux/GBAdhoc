#ifdef REWIND
/* ---- rewind: measurement wiring (PROTOTYPE; docs/REWIND.md) ---------------
 * Harness keys only, no menu yet.  The snapshots, the ring, the restores and
 * the re-runs are the real thing; what is NOT here yet is the picture: this
 * console's frames come from the second core's renderer, not from the core's
 * video callback, so nothing is captured and the screen simply holds while a
 * script rewinds (fe_host_rewind_opts.no_capture).
 *
 *   rewind            = 0|1   record (0 = nothing reserved, nothing runs)
 *   rewind_mib        = 1..8  megabytes of history, raw (default 2: the
 *                             player's Rewind length with rewind_product,
 *                             Medium unless changed; 2 in the lab
 *                             configuration too)
 *   rewind_interval   = N     frames between snapshots (default 6)
 *   rewind_burst      = N     most frames a rewind tick may run (default 2)
 *   rewind_full_flush = 0|1   a restore empties both translation caches, as
 *                             a state load does (default 0: ROM cache kept)
 *   rewind_ff         = 0|1   record fast-forwarded frames (default 1)
 *   rewind_save_quiet = N     frames the save chip (or the link port) must
 *                             have been left alone before play may continue
 *                             from a frame a rewind stops on (default 60)
 *   rewind_rlog_bytes = N     the log of save bytes the game read, served
 *                             again to re-run frames (default 131072)
 *   rewind_sav_mutant = N     TESTS ONLY: 1 re-runs read the live save, 2
 *                             re-runs write the save, 4 no snap
 * (The save is permanent under rewind, docs/REWIND.md 17: there is no
 * rewind_cross_save any more.)
 * Capture optimisations, each its own key so they can be A/B'd (all default
 * 0 = the first version's behaviour):
 *   rewind_inplace    = 0|1   compare the big arrays where they lie instead
 *                             of serialising the whole image first
 *   rewind_dirty      = 0..3  (with rewind_inplace) do not read pages
 *                             nothing wrote to since the last snapshot:
 *                             1 = VRAM, 2 = EWRAM, 3 = both
 *   rewind_pace_ms    = N     a snapshot needs N ms of wall clock since the
 *                             last one as well as rewind_interval frames, so
 *                             fast-forward takes them no faster than 1x does
 *                             (0 = off; 100 = the 1x rate at interval 6)
 *   rewind_vmem       = 0|1   the rewind's memory comes from the end of the
 *                             volatile pool instead of the heap (history
 *                             then starts again after a sleep)
 * Checks and measurement:
 *   rewind_verify     = 0|1   after every snapshot compare the whole machine
 *                             with the reference; EVT rewind_verify_fail and
 *                             a nonzero verify= at exit mean a written page
 *                             was not marked.  Costs a full compare each.
 *   rewind_gprof      = 0|1   (rewind_inplace = 0) EVT rewind_gprof: the
 *                             serialised gather's clear / save / sound times
 * Test instruments (slow: a line to the log folder every frame):
 *   rewind_hash       = 0|1   log/rewind_hash.txt for tools/rewind_check.py
 *   rewind_redo       = 0|1   replay rewound frames with their logged inputs
 *   rewind_fuzz_from  = F, rewind_fuzz_seed = S   buttons are a function of
 *                             the frame number from frame F on
 * and the autopilot step `rewind N` holds the control for N ticks.
 * Lines: EVT rewind_reserve / rewind_enable at boot, EVT rewind_win on the
 * heartbeat cadence, EVT rewind_gesture after each scripted rewind,
 * EVT rewind_stats / rewind_use / rewind_tick at exit. */
static int g_rw_want, g_rw_on;
static int g_rw_gb;             /* the setting is on, the game is GB / GBC */
/* The cadence of a rewind: `s` frames back every `p` display ticks, chosen
 * so that the game can be re-run as fast as it is shown (psp_rewind_tick). */
static struct
{
   int      on, force_p, force_s, speed_max;
   int      p, s;           /* in force */
   int      phase;          /* ticks since the last tick that showed */
   int      level;          /* the speed asked for: 1, 2, 4 */
   int      bad;            /* recent ticks that fell short, weighted */
   uint32_t scale;          /* the cost model, x100: raised when it was wrong */
   unsigned shows, shorts, demotes, changes, ticks, stepped;
   unsigned t_ticks, t_frames;   /* since the pace in force began */
   unsigned t_shows, t_shorts;
   unsigned last_us;        /* when the last held loop began */
   unsigned fail;           /* speed x1000 shown not to hold, this hold */
   unsigned held_ok;        /* fastest speed x1000 that has held, this game */
   unsigned cap;            /* frames per 1000 ticks the game has shown it
                             * can be re-run at (0 = not known) */
   int      first_p, first_s;
} g_rwc;
#define RW_SOUND_DEFAULT 2
static int g_rw_vmem;           /* key: live in the volatile pool */
static int g_rw_pool;           /* the rewind is a tenant of the pool now */
static int g_rw_asleep;         /* suspended for a sleep: resume on wake */
static int g_rw_held;           /* the control's level, this loop */
static fe_host_rewind_opts g_rw_opts;
#ifdef GPSP_PERF_RIG
static char g_rw_hash_path[192], g_rw_dump_dir[192];
#endif

/* ---- the pictures (fe_host_rewind_opts.pix_begin / pix_end / show) ------- */
static const uint16_t *g_rw_show;   /* the slot on screen while rewinding */
static int g_rw_showing;            /* a rewind is on screen */

/* Before every frame the rewind runs or records: where its picture goes.
 * 1 = the engine draws it there (g_rw_slot, taken by the post); 0 = the
 * engine is down, fe_host copies the core's own picture into the slot. */
static int psp_rw_pix_begin(uint16_t *slot)
{
   g_rw_slot_posted = 0;
   if (g_me_rend)
   {
      g_rw_slot = slot;
      return 1;
   }
   g_rw_slot = NULL;
   if (slot)
      g_rw_flush_posts = 32;    /* this CPU is writing slots through its cache */
   return 0;
}

static int psp_rw_pix_end(void)
{
   int posted = g_rw_slot_posted;
   g_rw_slot = NULL;
   g_rw_slot_posted = 0;
   return posted;
}

/* While rewinding: this slot is the picture (NULL: the frame has none, keep
 * what is up).  It is drawn at the top of the next loop, where the engine's
 * frame would be (psp_rewind_present). */
static void psp_rw_show(const uint16_t *slot)
{
   if (!slot)
      return;
   if (g_me_rend)
   {
      /* Still being drawn?  Then wait for it: a few milliseconds at most,
       * once per window, instead of showing half a frame. */
      if (g_mer_pending >= 0 && g_mer_stage[g_mer_pending] == slot)
      {
         unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();
         while (!me_host_idle() &&
                (unsigned)sceKernelGetSystemTimeLow() - t0 < 12000u)
            ;
         g_rwm.show_waits++;
         g_rwm.show_us += (unsigned)sceKernelGetSystemTimeLow() - t0;
      }
   }
   else
      /* this CPU filled it: the GE reads memory, not our cache */
      sceKernelDcacheWritebackRange(slot, MER_STAGE_PITCH * 161 * 2);
   g_rw_show = slot;
}

/* Loop top, in the place of the engine's present.  1 = drawn. */
static int psp_rewind_present(void)
{
   if (!g_rw_showing || !g_rw_show)
      return 0;
   vid_draw_prestaged(g_rw_show, 240, 160);
   g_drew = 1;
   return 1;
}

/* Play resumes: the frame on screen stays up until the engine's next one
 * lands.  The ready stage is pointed at the slot that is showing (it is the
 * frame play resumes from); the stage a re-run was last drawn into is the
 * other one and is never presented. */
static void psp_rewind_show_end(void)
{
   if (g_me_rend && g_rw_show)
   {
      int r = g_mer_ready >= 0 ? g_mer_ready : (g_mer_pending == 0 ? 1 : 0);
      if (r != g_mer_pending)
      {
         g_mer_stage[r] = (uint16_t *)g_rw_show;
         g_mer_ready = r;
         g_mer_last_presented = -1;
      }
   }
   g_rw_show = NULL;
   g_rw_showing = 0;
}

/* What the Settings toggle turns on: every optimisation, the pictures, the
 * volatile pool.  `mb` = megabytes of history: the player's "Rewind length"
 * (config_psp.h: Short 1; Medium 2, the default; Long 4).  The harness key
 * rewind_mib overrides it with any size, as before. */
static void psp_rewind_product(int mb)
{
   memset(&g_rw_opts, 0, sizeof(g_rw_opts));
   g_rw_opts.ring_mib        = (uint32_t)mb;
   g_rw_opts.interval        = 6;
   g_rw_opts.burst           = 8;
   g_rw_opts.tick_budget_us  = 14183;
   g_rw_opts.save_quiet      = 60;
   g_rw_opts.ff_rewindable   = 1;
   g_rw_opts.inplace         = 1;
   g_rw_opts.dirty           = 3;
   g_rw_opts.fast_restore    = 1;
   g_rw_opts.clock_frames    = 1;
   g_rw_opts.pace_us         = 100000;
   g_rw_opts.slack_margin_us = 2500;
   g_rw_opts.max_span        = 48;
   /* The history target (about ten seconds a megabyte whatever the scene:
    * snapshots up to 24 frames apart while they are large) is OFF in the
    * 4.0.0 beta: on a PSP-3000 it gave an intermittent ~0.8 s hitch after
    * the control was released (catch-up of 46-52 ticks against 1-3 with it
    * off; docs/REWIND.md 14), and the owner prefers smooth over long.
    * Harness rewind_target_s = N turns it on.  The Settings row therefore
    * names a length (Short / Medium / Long), not seconds. */
   (void)mb;
   g_rw_opts.target_s        = 0;
   g_rw_opts.gap_max         = 18;
   g_rw_vmem                 = 1;
}

static void psp_rewind_read_ini(void)
{
   int want = (int)HARNESS_INT("rewind", -1);
   int product = (int)HARNESS_INT("rewind_product", want < 0);

   /* No harness key: the player's setting, with everything on.  With one:
    * the stage-2 behaviour (every optimisation off) unless rewind_product
    * asks for the shipped configuration; single keys override either. */
   if (product)
      psp_rewind_product(g_pcfg.rewind_mb);
   else
   {
      memset(&g_rw_opts, 0, sizeof(g_rw_opts));
      g_rw_opts.ring_mib = 2;
      g_rw_opts.interval = 6;
      g_rw_opts.burst = 2;
      g_rw_opts.save_quiet = 60;
      g_rw_opts.ff_rewindable = 1;
      g_rw_vmem = 0;
   }
   g_rw_want = want < 0 ? g_pcfg.rewind : want != 0;
#define RW_KEY(field, key) \
   g_rw_opts.field = (uint32_t)HARNESS_INT(key, (long)g_rw_opts.field)
#define RW_FLAG(field, key) \
   g_rw_opts.field = HARNESS_INT(key, g_rw_opts.field) != 0
   RW_KEY(ring_mib, "rewind_mib");
   RW_KEY(interval, "rewind_interval");
   RW_KEY(burst, "rewind_burst");
   RW_KEY(tick_budget_us, "rewind_tick_budget_us");
   RW_FLAG(full_flush_restore, "rewind_full_flush");
   RW_KEY(rlog_bytes, "rewind_rlog_bytes");
   g_rw_opts.sav_mutant = (int)HARNESS_INT("rewind_sav_mutant", 0);
   RW_FLAG(ff_rewindable, "rewind_ff");
   RW_KEY(save_quiet, "rewind_save_quiet");
   RW_FLAG(inplace, "rewind_inplace");
   g_rw_opts.dirty = (int)HARNESS_INT("rewind_dirty",
                                         g_rw_opts.dirty) & 3;
   RW_FLAG(verify, "rewind_verify");
   RW_FLAG(gather_prof, "rewind_gprof");
   RW_FLAG(fast_restore, "rewind_fast_restore");
   RW_FLAG(clock_frames, "rewind_clock");
   RW_KEY(slack_margin_us, "rewind_slack_us");
   RW_KEY(max_span, "rewind_max_span");
   g_rw_vmem = HARNESS_INT("rewind_vmem", g_rw_vmem) != 0;
   /* The even cadence (psp_rewind_tick).  rewind_cadence = 0: the first
    * version's ramp, a step every tick.  rewind_period / rewind_stride:
    * force "stride frames back every period ticks" instead of choosing.
    * rewind_speed_max: 1, 2 or 4.
    * rewind_sound = 2 the frames gone over are heard backwards (only ever
    * at one picture a frame a tick), 1 silence, 0 silence and our own sound
    * synthesis is left out of the frames a held rewind re-runs. */
   g_rwc.on = HARNESS_INT("rewind_cadence", product) != 0;
   g_rwc.force_p = (int)HARNESS_INT("rewind_period", 0);
   g_rwc.force_s = (int)HARNESS_INT("rewind_stride", 0);
   g_rwc.speed_max = (int)HARNESS_INT("rewind_speed_max", 4);
   /* rewind_ring = 1 gauge (default), 2 gauge + time left, 3 two rings,
    * 0 the first version (psp_rewind_hud).
    * rewind_head_skip = 0: frames re-run without a picture are still
    * captured for the renderer, as before. */
   g_rw_ring = (int)HARNESS_INT("rewind_ring", 2);
   /* rewind_target_s: seconds of history to aim for (0 = snapshots always
    * rewind_interval apart, as before); rewind_gap_max: the most frames
    * between two snapshots it may stretch to. */
   g_rw_opts.target_s = (uint32_t)HARNESS_INT(
                           "rewind_target_s", (long)g_rw_opts.target_s);
   g_rw_opts.gap_max = (uint32_t)HARNESS_INT(
                           "rewind_gap_max", (long)g_rw_opts.gap_max);
   fe_host_rewind_set_head_skip(
      HARNESS_INT("rewind_head_skip", 1) != 0);
   g_rw_opts.sound_mode = 2 - (int)HARNESS_INT("rewind_sound",
                                                  RW_SOUND_DEFAULT);
   if (g_rw_opts.sound_mode < 0 || g_rw_opts.sound_mode > 2)
      g_rw_opts.sound_mode = 0;
   g_rw_opts.pace_us = (uint32_t)HARNESS_INT("rewind_pace_ms",
                          (long)(g_rw_opts.pace_us / 1000u)) * 1000u;
#undef RW_KEY
#undef RW_FLAG
   /* The pictures.  rewind_pix = 0: keep none, the screen holds while
    * rewinding (stage 2). */
   g_rw_opts.no_capture = HARNESS_INT("rewind_pix", 1) == 0;
   g_rw_opts.pix_pitch = MER_STAGE_PITCH;
   g_rw_opts.pix_rows  = 161;
   g_rw_opts.pix_begin = psp_rw_pix_begin;
   g_rw_opts.pix_end   = psp_rw_pix_end;
   g_rw_opts.show      = psp_rw_show;
#ifdef GPSP_PERF_RIG
   /* A test instrument of harness/lab builds (tools/rewind_*.sh build
    * them); a release has no rewind_hash, nor its audio tee (fe_host.c). */
   if (HARNESS_INT("rewind_hash", 0))
   {
      snprintf(g_rw_hash_path, sizeof(g_rw_hash_path), "%s/log/rewind_hash.txt",
               g_dir_base);
      g_rw_opts.hash_path = g_rw_hash_path;
      /* rewind_dump_frame = N: log/state_N_<k>.bin each time frame N runs */
      g_rw_opts.dump_frame =
         (uint32_t)HARNESS_INT("rewind_dump_frame", 0);
      snprintf(g_rw_dump_dir, sizeof(g_rw_dump_dir), "%s/log", g_dir_base);
      g_rw_opts.dump_dir = g_rw_dump_dir;
   }
#endif
   g_rw_opts.redo_inputs = HARNESS_INT("rewind_redo", 0) != 0;
   g_rw_opts.fuzz_from = (uint32_t)HARNESS_INT(
                                                  "rewind_fuzz_from", 0);
   g_rw_opts.fuzz_seed = (uint32_t)HARNESS_INT(
                                                  "rewind_fuzz_seed", 1);
}

/* ---- the rewind as a tenant of the volatile pool (see vm_lock) ------------ */
static int psp_rewind_pool_tenant(void)
{
   return g_rw_pool || g_rom_vm_lend;
}

/* Before the game loads, with the rewind's decision made. */
static void psp_rom_pool_offer(void)
{
   extern u8 *gamepak_spare_pool3;
   extern u32 gamepak_spare_pool3_bytes;
   void *base = NULL;
   int size = 0;
   gamepak_spare_pool3 = NULL;
   gamepak_spare_pool3_bytes = 0;
   g_rom_vm_lend = g_rom_vm_asleep = 0;
   if (!HARNESS_INT("rom_vmem", 0) || g_rw_want ||
       g_pcfg.console != FE_CONSOLE_GBA)
      return;
   if (vm_lock(&base, &size) != 0 || !base || size <= PSP_VM_ENGINE_BYTES)
   {
      fe_evt("rom_vmem unavailable size=%d", size);
      return;
   }
   gamepak_spare_pool3 = (u8 *)base + PSP_VM_ENGINE_BYTES;
   gamepak_spare_pool3_bytes = (u32)(size - PSP_VM_ENGINE_BYTES);
   g_rom_vm_lend = 1;
   fe_evt("rom_vmem base=%08x bytes=%u", (unsigned)gamepak_spare_pool3,
          (unsigned)gamepak_spare_pool3_bytes);
}

/* The game is gone, and its ROM cache with it. */
static void psp_rom_pool_over(void)
{
   extern u8 *gamepak_spare_pool3;
   extern u32 gamepak_spare_pool3_bytes;
   gamepak_spare_pool3 = NULL;
   gamepak_spare_pool3_bytes = 0;
   g_rom_vm_lend = g_rom_vm_asleep = 0;
}

/* Offer the rewind its end of the pool.  Before fe_host_rewind_reserve at
 * boot; returns the block for fe_host_rewind_resume after a wake. */
static void *psp_rewind_pool_offer(size_t *bytes)
{
   void *base = NULL;
   int size = 0;
   *bytes = 0;
   g_rw_pool = 0;
   if (!g_rw_vmem)
      return NULL;
   if (vm_lock(&base, &size) != 0 || !base || size <= PSP_VM_ENGINE_BYTES)
   {
      fe_evt("rewind_pool unavailable size=%d (heap)", size);
      return NULL;
   }
   g_rw_pool = 1;
   *bytes = (size_t)(size - PSP_VM_ENGINE_BYTES);
   fe_evt("rewind_pool base=%08x bytes=%u engine_keeps=%u",
          (unsigned)base + PSP_VM_ENGINE_BYTES, (unsigned)*bytes,
          (unsigned)PSP_VM_ENGINE_BYTES);
   return (char *)base + PSP_VM_ENGINE_BYTES;
}

/* The pool is about to go back to the OS (a sleep).  The main loop is
 * parked between frames, or is not emulating at all.
 *
 * A heap-backed rewind keeps its history across the sleep -- unless the
 * machine stands inside an old save: its hold (psp_rewind_pwr) would then
 * keep the suspend off until the park's hold gave up, and the slide would
 * be lost.  With the loop parked it is ended here as the pool's is, and
 * fe_host_rewind_suspend decides it (vetted frame, or the game is reset). */
/* @extract:begin fn:psp_rewind_pool_lost */
static void psp_rewind_pool_lost(void)
{
   if (g_rom_vm_lend && !g_rom_vm_asleep)
   {
      gamepak_vol_drop();
      g_rom_vm_asleep = 1;
      fe_evt("rom_vmem dropped");
   }
   if (!g_rw_pool &&
       !(g_pwr_parked && g_rw_on && fe_host_rewind_unvetted()))
      return;
   if (g_rw_on)
   {
      fe_host_rewind_suspend();
      g_rw_on = 0;
      g_rw_asleep = 1;
   }
   /* the pictures were in the pool too */
   g_rw_showing = 0;
   g_rw_show = NULL;
   g_rw_slot = NULL;
   g_rw_pool = 0;
}
/* @extract:end fn:psp_rewind_pool_lost */

/* The last resort of docs/REWIND.md 22 was taken (the game was reset
 * because a sleep ended a rewind with the machine inside an old save): say
 * so once.  The reset itself happens at the first frame after the park,
 * which may be before the machine has even gone to sleep. */
static void psp_rewind_reset_said(void)
{
   static unsigned seen;
   unsigned n = fe_host_rewind_resets();
   if (n != seen)
   {
      osd_toast("Rewind interrupted: game restarted from its save");
      ctl_turbo_stop(&g_turbo);   /* a restarted game has no auto-fire */
   }
   seen = n;
}

/* After a wake, once the engine is rebuilt. */
static void psp_rewind_pool_back(void)
{
   void *base;
   size_t bytes;
   if (g_rom_vm_asleep)
   {
      /* The same pool again, or those pages stay out of the cache. */
      int size = 0;
      void *b = NULL;
      extern u8 *gamepak_spare_pool3;
      g_rom_vm_asleep = 0;
      if (vm_lock(&b, &size) == 0 && b &&
          (u8 *)b + PSP_VM_ENGINE_BYTES == gamepak_spare_pool3)
      {
         gamepak_vol_back();
         fe_evt("rom_vmem back");
      }
      else
      {
         g_rom_vm_lend = 0;
         fe_evt("rom_vmem lost size=%d", size);
      }
   }
   if (!g_rw_asleep)
      return;
   g_rw_asleep = 0;
   base = psp_rewind_pool_offer(&bytes);
   g_rw_on = fe_host_rewind_resume(base, bytes) == 0;
   if (!g_rw_on)
      g_rw_pool = 0;
   fe_evt("rewind_wake on=%d pool=%d", g_rw_on, g_rw_pool);
   psp_rewind_reset_said();
}

/* A SLEEP THAT TORE NOTHING DOWN (power_cb: the loop did not park in time,
 * or the engine was busy, or standby = 0) left the rewind in the volatile
 * pool across the suspend, and the pool is the OS's to use while the machine
 * sleeps.  Nothing in it can be believed afterwards: the reference image,
 * the ring, the read log and the picture windows are all there.  The history
 * therefore starts again, in place: suspend drops every pointer, resume
 * copies the reference from the machine as it is and empties the ring.
 * A rewind that was in progress cannot be vetted from that memory either
 * (fe_host_rewind_distrust): the game is reset.  Main thread, between
 * frames, before the rewind is ticked or a frame is recorded.
 * Which loops cannot park: docs/REWIND.md 23. */
static void psp_rewind_pool_restart(void)
{
   void *base;
   size_t bytes;
   /* ROM pages kept across that sleep cannot be believed either: read them
    * again as play reaches them. */
   if (g_rom_vm_lend)
   {
#ifdef GPSP_PERF_RIG
      if (!g_ss_no_restart)
#endif
      {
         gamepak_vol_drop();
         gamepak_vol_back();
         fe_evt("rom_vmem reread why=sleep_without_teardown");
      }
   }
   if (!g_rw_on || !g_rw_pool)
   {
      fe_evt("rewind_pool_restart skipped on=%d pool=%d", g_rw_on, g_rw_pool);
      return;
   }
#ifdef GPSP_PERF_RIG
   if (g_ss_no_restart)
   {
      fe_evt("rewind_pool_restart MUTANT: the history is left as it was");
      return;
   }
#endif
   if (g_rw_showing)
      psp_rewind_show_end();
   g_rw_showing = 0;
   g_rw_show = NULL;
   g_rw_slot = NULL;
   fe_host_rewind_distrust();
   fe_host_rewind_suspend();
   base = psp_rewind_pool_offer(&bytes);
   g_rw_on = fe_host_rewind_resume(base, bytes) == 0;
   if (!g_rw_on)
      g_rw_pool = 0;
   psp_rewind_reset_said();
   fe_evt("rewind_pool_restart why=sleep_without_teardown on=%d pool=%d",
          g_rw_on, g_rw_pool);
}

#ifdef GPSP_PERF_RIG
/* Harness: what the OS may do to a pool that was kept across a sleep. */
static void sleep_sim_trash_pool(void)
{
   unsigned char *lo = (unsigned char *)fe_host_rewind_pool_low();
   unsigned char *hi = (unsigned char *)g_vm_base + g_vm_size;
   if (g_rom_vm_lend)
   {
      lo = (unsigned char *)g_vm_base + PSP_VM_ENGINE_BYTES;
      memset(lo, 0xA5, (size_t)(hi - lo));
      fe_evt("sleep_sim trash_pool rom from=%08x bytes=%u", (unsigned)lo,
             (unsigned)(hi - lo));
      return;
   }
   if (!g_rw_pool || !lo || !g_vm_base || lo >= hi)
   {
      fe_evt("sleep_sim trash_pool skipped pool=%d", g_rw_pool);
      return;
   }
   memset(lo, 0xA5, (size_t)(hi - lo));
   fe_evt("sleep_sim trash_pool from=%08x bytes=%u", (unsigned)lo,
          (unsigned)(hi - lo));
}
#endif

/* me_rend_init has carved the engine's buffers: they must end below the
 * rewind's part.  They do by a wide margin today; if a later engine ever
 * grows past the boundary the rewind leaves the pool rather than share a
 * byte of it. */
static void psp_rewind_pool_check(void)
{
   unsigned limit = (unsigned)g_vm_base + PSP_VM_ENGINE_BYTES;
   unsigned low = (unsigned)fe_host_rewind_pool_low();
   (void)low;
   fe_evt("vmem engine_top=%08x boundary=%08x rewind_low=%08x %s",
          g_vm_eng_top, limit, low,
          g_vm_eng_top <= limit ? "ok" : "CONFLICT");
   if (g_vm_eng_top > limit && g_rom_vm_lend)
   {
      /* (no ROM page may share a byte with the engine) */
      gamepak_vol_drop();
      g_rom_vm_lend = 0;
      fe_evt("rom_vmem lost engine_top=%08x", g_vm_eng_top);
   }
   if (g_vm_eng_top > limit && g_rw_pool)
   {
      /* (From inside me_rend_init: no frame can be run here to land a
       * rewind.  suspend applies the unlanded rule -- on from the machine's
       * frame if that is one play may continue from, else the game is reset
       * -- and abandon carries it out; docs/REWIND.md 22.) */
      if (g_rw_on)
      {
         fe_host_rewind_suspend();
         fe_host_rewind_abandon();
      }
      g_rw_on = 0;
      g_rw_pool = 0;
      g_rw_asleep = 0;
   }
}

/* ---- the control and the ring (docs/REWIND.md, 10.7) ----------------------
 * The designer's "Big ring", alone: a hairline ring round the middle of the
 * picture that draws itself on while the control is held and off when it is
 * let go; its arc is the history left, a bright head with a short tail runs
 * counter-clockwise as fast as the rewind goes, and one line under it says
 * how far back.  No arrows, no button glyph.
 *
 * Everything is a scalar that moves a step a tick towards where it should
 * be, so any pattern of taps only ever turns a motion round from where it
 * is: nothing restarts, nothing flashes.
 *   intensity  0..1, +1/8 a tick held, -1/12 let go.  The game holds its
 *              frame while it rises and the rewind starts at 1, so a fresh
 *              press takes 8 ticks to bite and a tap straight after a
 *              release only the few it has fallen.
 *   ui         the readout and the dim: +1/6 held; it lingers 36 ticks
 *              after a release, then -1/10.
 *   p          the ring's draw-on, +-1/6 towards held.
 *   speed      x1 for the first 0.8 s of a hold, then up to x4 at 3.5 s.
 *              What is shown (head speed, tail, the "x2") is what the
 *              rewind actually managed, not what was asked. */
static struct
{
   float    intensity, ui, p, head, sp, acc;
   int      linger, held_ticks;
   uint32_t anchor;        /* the frame the readout counts back from */
   uint32_t avail0;        /* history there was when the ring came up */
   uint32_t now, avail;
   int      limit, limit_why, denied;
   float    gauge;         /* share of the history still to go back to */
   int      was_held;
} g_rwh;

#define RWH_PI2 6.2831853f

/* periods: display periods this loop took (1 unless a tick ran long). */
static void psp_rewind_hud_step(int held, int rewinding, uint32_t stepped,
                                uint32_t periods,
                                const fe_host_rewind_info *inf)
{
   float want = held ? 1.0f : 0.0f, d, frac;
   if (held)
   {
      g_rwh.ui += 1.0f / 6.0f;
      if (g_rwh.ui > 1.0f)
         g_rwh.ui = 1.0f;
      g_rwh.linger = 0;
   }
   else if (++g_rwh.linger > 36)
   {
      g_rwh.ui -= 0.1f;
      if (g_rwh.ui < 0.0f)
         g_rwh.ui = 0.0f;
   }
   d = want - g_rwh.p;
   if (d > 1.0f / 6.0f)  d = 1.0f / 6.0f;
   if (d < -1.0f / 6.0f) d = -1.0f / 6.0f;
   g_rwh.p += d;
   if (g_rwh.ui <= 0.0f && !held)
   {
      g_rwh.anchor = inf->now_frame;
      g_rwh.avail0 = inf->avail_frames;
   }
   if (inf->now_frame > g_rwh.anchor)
      g_rwh.anchor = inf->now_frame;
   if (held && !g_rwh.was_held)
   {
      /* a new hold: the lap is what can be gone back NOW, however little,
       * and the dot is at its start again */
      g_rwh.avail0 = inf->avail_frames;
      g_rwh.gauge = 1.0f;
   }
   g_rwh.was_held = held;
   if (inf->avail_frames > g_rwh.avail0)
      g_rwh.avail0 = inf->avail_frames;
   g_rwh.now = inf->now_frame;
   g_rwh.avail = inf->avail_frames;
   g_rwh.limit = held && inf->at_limit;
   g_rwh.limit_why = inf->limit_reason;
   /* the gauge: the share of the history still behind the frame on screen,
    * eased so that a step of several frames is a glide, not a jump */
   frac = g_rwh.avail0 ? (float)g_rwh.avail / (float)g_rwh.avail0 : 0.0f;
   if (frac > 1.0f) frac = 1.0f;
   if (g_rwh.ui <= 0.0f && !held)
      g_rwh.gauge = frac;
   else
      g_rwh.gauge += (frac - g_rwh.gauge) * 0.25f;
   if (g_rwc.on)
   {
      /* an even cadence shows on some ticks only: the head runs at the
       * pace, smoothly, and the pace is the mean of what was managed */
      float per = (float)(periods ? periods : 1);
      if (held && g_rwh.intensity >= 1.0f)
         g_rwh.sp += ((rewinding ? (float)stepped / per : 0.0f) - g_rwh.sp) *
                     (periods > 1 ? 0.2f : 0.1f);
      else
         g_rwh.sp += (1.0f - g_rwh.sp) * 0.2f;
      if (rewinding && held && g_rwh.intensity >= 1.0f)
         g_rwh.head -= 0.028f * g_rwh.sp * per;   /* counter-clockwise */
   }
   else
   {
   if (rewinding && stepped)
      g_rwh.head -= 0.028f * (float)stepped;      /* counter-clockwise */
   g_rwh.sp += ((rewinding ? (float)stepped : 1.0f) - g_rwh.sp) * 0.2f;
   }
   if (g_rwh.head < -RWH_PI2)
      g_rwh.head += RWH_PI2;
}

/* Over the game picture, after it is drawn.
 *
 * g_rw_ring picks how the ring says how much history is left (the owner's
 * request; all of them are the plain ring, counter-clockwise, no arrows):
 *   1  GAUGE.  The ring is the history there was when the hold began.  It
 *      is fully lit then; the lit part ends at a bright dot that leaves
 *      the BOTTOM counter-clockwise as the game runs backwards, and what
 *      is behind the dot is a faint track.  One lap is all of it, however
 *      short; every new hold starts at the bottom again with a lap equal
 *      to what is available then.  Dot back at the bottom = ring empty:
 *      "Start of history".
 *   2  GAUGE + TIME (the owner's choice, the default).  The same, and the
 *      line under it reads "-0:05   0:27 left" -- or "0:27 to state load", "to cheat change", "to link use", "to sleep" when
 *      that, not the size of the history, is where it ends.
 *   3  TWO RINGS.  The moving ring of the first version (a head that keeps
 *      turning at the speed of the rewind), and inside it a thin gauge ring
 *      that drains as in 1.
 *   0  the first version: the lit arc turns with the head, its length is
 *      the history left. */
static void psp_rewind_hud(void)
{
   const float cx = 240.0f, cy = 130.0f, r = 92.0f;
   /* The owner's rule for the gauge: the dot always starts at the BOTTOM,
    * and one lap counter-clockwise back to the bottom is all the history
    * there was when this hold began (psp_rewind_hud_step, avail0). */
   const float top = 1.5707963f;
   const uint16_t white = 0xFFFF, grey = 0x8C72, soft = 0xCE59;
   float p = g_rwh.p, ui = g_rwh.ui, frac, len;
   char t[64];
   int a = (int)(ui * 255.0f), limit = g_rwh.limit;

   if (g_rwh.denied)
   {
      const char *msg = g_rwh.denied == 2
         ? "Rewind isn't available in Game Boy games yet"
         : g_rwh.denied == 3 ? "Rewind isn't available right now"
                             : "Rewind is off during link play";
      vid_overlay_begin(0);
      vid_arc(cx, cy, r, 3.0f, 0.0f, RWH_PI2, white, 90, 90, 0);
      vid_seg(cx - r * 0.7f, cy + r * 0.7f, cx + r * 0.7f, cy - r * 0.7f,
              3.0f, white, 255);
      vid_text((480 - vid_text_w(msg)) / 2, (int)(cy + r) + 12, msg, white);
      vid_overlay_end();
      return;
   }
   if (p <= 0.01f && ui <= 0.01f)
      return;
   vid_overlay_begin(0);
   vid_rect(0, 0, 480, 272, 0x0000, (int)(ui * 40.0f));
   if (g_rw_ring == 0)
   {
      frac = g_rwh.avail0 ? (float)g_rwh.avail / (float)g_rwh.avail0 : 0.0f;
      if (frac < 0.02f) frac = 0.02f;
      if (frac > 1.0f)  frac = 1.0f;
      len = p * RWH_PI2 * frac;
      vid_arc(cx, cy, r, 3.0f, 0.0f, RWH_PI2, white, (int)(36.0f * p),
              (int)(36.0f * p), 0);
      vid_arc(cx, cy, r, 3.0f, g_rwh.head, g_rwh.head + len,
              limit ? grey : white, (int)(255.0f * p), (int)(255.0f * p), 0);
      if (!limit)
      {
         float L = 0.05f * g_rwh.sp * 6.0f;       /* the tail: six short steps */
         vid_arc(cx, cy, r, 6.0f, g_rwh.head, g_rwh.head + L, white,
                 (int)(128.0f * p), 0, 1);
         vid_disc(cx + cosf(g_rwh.head) * r, cy + sinf(g_rwh.head) * r, 4.5f,
                  white, (int)(255.0f * p));
      }
   }
   else
   {
      /* The gauge.  Angles grow clockwise on the screen; the dot leaves
       * the top counter-clockwise, so it stands at top - used, and the lit
       * part is the rest of the way round, from there back to the top. */
      float gr = g_rw_ring == 3 ? r - 9.0f : r;
      float gw = g_rw_ring == 3 ? 2.0f : 3.0f;
      float used, dot;
      frac = g_rwh.gauge;
      if (frac < 0.0f) frac = 0.0f;
      if (frac > 1.0f) frac = 1.0f;
      used = RWH_PI2 * (1.0f - frac);
      dot = top - used;
      vid_arc(cx, cy, gr, gw, 0.0f, RWH_PI2, white, (int)(40.0f * p),
              (int)(40.0f * p), 0);
      if (frac > 0.004f)
         vid_arc(cx, cy, gr, gw, top - RWH_PI2 * p, top - RWH_PI2 * p +
                 (RWH_PI2 * frac) * p, limit ? grey : white,
                 (int)(255.0f * p), (int)(255.0f * p), 0);
      if (g_rw_ring == 3)
      {
         /* the moving ring outside it, as before */
         vid_arc(cx, cy, r, 3.0f, 0.0f, RWH_PI2, white, (int)(36.0f * p),
                 (int)(36.0f * p), 0);
         if (!limit)
         {
            float L = 0.05f * g_rwh.sp * 6.0f + 0.5f;
            vid_arc(cx, cy, r, 3.0f, g_rwh.head, g_rwh.head + L, white,
                    (int)(255.0f * p), 0, 1);
            vid_disc(cx + cosf(g_rwh.head) * r, cy + sinf(g_rwh.head) * r,
                     4.0f, white, (int)(255.0f * p));
         }
         vid_disc(cx + cosf(dot) * gr, cy + sinf(dot) * gr, 3.0f,
                  limit ? grey : white, (int)(255.0f * p));
      }
      else if (!limit)
      {
         /* the dot, and behind it (where it has just been) a short tail
          * as long as the rewind is fast */
         float L = 0.05f * g_rwh.sp * 6.0f;
         if (L > used)
            L = used;
         vid_arc(cx, cy, r, 6.0f, dot, dot + L, white, (int)(150.0f * p),
                 0, 1);
         vid_disc(cx + cosf(dot) * r, cy + sinf(dot) * r, 4.5f, white,
                  (int)(255.0f * p));
      }
   }
   if (limit)
   {
      /* a short history has a visible cause */
      const char *why =
         g_rwh.limit_why == FE_RW_LIMIT_STATE  ? "State loaded here" :
         g_rwh.limit_why == FE_RW_LIMIT_CHEAT  ? "Cheats changed here" :
         g_rwh.limit_why == FE_RW_LIMIT_SERIAL ? "Link port used here" :
         g_rwh.limit_why == FE_RW_LIMIT_SLEEP  ? "Woke from sleep here" :
         g_rwh.limit_why == FE_RW_LIMIT_OFF    ? "Rewind was off before this" :
                                                 "Start of history";
      static const char rel[] = "release to play";
      vid_text_a((480 - vid_text_w(why)) / 2, (int)(cy + r) + 12, why, white, a);
      vid_text_a((480 - vid_text_w(rel)) / 2, (int)(cy + r) + 28, rel, soft, a);
   }
   else
   {
      /* how far back from where the first press of this gesture was */
      unsigned s = (unsigned)((float)(g_rwh.anchor - g_rwh.now) / 59.7275f);
      unsigned l = (unsigned)((float)g_rwh.avail / 59.7275f);
      int k = snprintf(t, sizeof(t), "-%u:%02u", s / 60, s % 60);
      /* (No pace tag: "x2" came and went with a game on the margin between
       * two paces, and the line is centred, so the readout jumped sideways
       * with it.) */
      if (g_rw_ring == 2)
         (void)snprintf(t + k, sizeof(t) - (size_t)k, "   %u:%02u %s",
                       l / 60, l % 60,
                       g_rwh.limit_why == FE_RW_LIMIT_STATE  ? "to state load" :
                       g_rwh.limit_why == FE_RW_LIMIT_CHEAT  ? "to cheat change" :
                       g_rwh.limit_why == FE_RW_LIMIT_SERIAL ? "to link use" :
                       g_rwh.limit_why == FE_RW_LIMIT_SLEEP  ? "to sleep" :
                                                               "left");
      vid_text_a((480 - vid_text_w(t)) / 2, (int)(cy + r) + 12, t, white, a);
   }
   vid_overlay_end();
}

/* ---- the cadence -----------------------------------------------------------
 * A rewind shows frames it has to re-run first, a window of `interval` at a
 * time: restore the snapshot below, run forward.  Shown faster than they
 * can be re-run, the picture goes in bursts and stands still in between,
 * which is what looks bad -- not the speed.  So the pace is chosen from what
 * the game costs, and then kept: `s` frames back every `p` display periods,
 * where only the frame landed on is drawn (the others are re-run without a
 * picture, which is the cheaper kind).
 *
 * A pace FITS when the work for one step -- one drawn frame, s - 1 undrawn
 * ones, their share of a restore -- takes less than the p periods the step
 * lasts (less RWC_SLACK_US for the loop's own work).  With p > 1 the tick
 * that shows is given the whole p periods to work in: it runs over the
 * display periods in between, in which the picture would stand anyway.
 * That is what lets a game whose frames are 9 ms be re-run three to two
 * periods instead of one to one.
 *
 * Among the paces that fit and are no faster than the hold asks for (x1 for
 * the first second, x2 to 2.5 s, x4 after), the fastest; at the same speed
 * the one with more pictures a second.  Distance before picture rate: a
 * heavy game gets x1 at 20 pictures a second rather than x1/2 at 30.
 *
 * The costs are measurements, so the model can be wrong.  If more than one
 * show in four finds its frames not ready over at least 48 periods, that
 * speed is ruled out for the rest of the hold -- and so is anything more
 * than 15% above what was actually managed meanwhile -- and the fastest
 * pace left that fits is taken: as far down as it has to go, in one step.  A speed that has held for 90
 * periods is remembered for the game: later holds may take it whatever the
 * model says, and are never started slower than it because of the model. */
#define RWC_PERIOD_US  16683u
#define RWC_SLACK_US   2500u

static int psp_rwc_fits(int p, int s, const fe_host_rewind_info *inf)
{
   uint32_t e  = inf->frame_us ? inf->frame_us : 9000u;
   uint32_t ep = inf->rerun_pic_us ? inf->rerun_pic_us : e;
   uint32_t eh = inf->rerun_head_us ? inf->rerun_head_us : e - e / 8;
   uint32_t rs = inf->restore_us ? inf->restore_us : 4000u;
   uint32_t n = g_rw_opts.interval ? g_rw_opts.interval : 6;
   uint32_t work = ep + (uint32_t)(s - 1) * eh + (uint32_t)s * rs / n;
   uint32_t pics = (n + (uint32_t)s - 1) / (uint32_t)s;
   uint32_t win = rs + pics * ep + (n - (pics < n ? pics : n)) * eh;
   if (work + RWC_SLACK_US > (uint32_t)p * RWC_PERIOD_US)
      return 0;
   /* And the window before must be whole before the one on screen is used
    * up (there are two buffers, so it cannot be begun earlier): a window's
    * work, and half a period for where in a tick it happens to start,
    * inside the time a window takes to show. */
   return win + RWC_PERIOD_US / 2 + RWC_SLACK_US <=
          n * (uint32_t)p * RWC_PERIOD_US / (uint32_t)s;
}

/* level: the speed the hold asks for (1, 2, 4). */
static void psp_rwc_choose(int level, const fe_host_rewind_info *inf,
                           const char *why)
{
   int p, s, bp = 4, bs = 1;
   (void)why;
   unsigned best = 0;

   if (g_rwc.force_p > 0 && g_rwc.force_s > 0)
   {
      bp = g_rwc.force_p;
      bs = g_rwc.force_s;
   }
   else
      for (p = 1; p <= 5; p++)
         for (s = 1; s <= 12; s++)
         {
            unsigned sp = (unsigned)s * 1000u / (unsigned)p;
            if (p > 3 && s > 3)
               continue;                  /* under 20 pictures a second only
                                           * for the slow paces */
            if (sp > (unsigned)level * 1000u)
               continue;                  /* faster than asked */
            if (g_rwc.fail && sp >= g_rwc.fail)
               continue;                  /* shown not to hold, this hold */
            if (!(g_rwc.held_ok && sp <= g_rwc.held_ok) &&
                !psp_rwc_fits(p, s, inf))
               continue;
            if (sp > best)                /* (p ascends: more pictures wins
                                           * a tie) */
            {
               best = sp;
               bp = p;
               bs = s;
            }
         }
   if (bp != g_rwc.p || bs != g_rwc.s)
   {
      fe_evt("rewind_tier p=%d s=%d why=%s frame_us=%u pic_us=%u head_us=%u "
             "restore_us=%u fail_x1000=%u held_x1000=%u", bp, bs, why,
             (unsigned)inf->frame_us, (unsigned)inf->rerun_pic_us,
             (unsigned)inf->rerun_head_us, (unsigned)inf->restore_us,
             g_rwc.fail, g_rwc.held_ok);
      g_rwc.changes++;
      g_rwc.t_ticks = g_rwc.t_frames = 0;
      g_rwc.t_shows = g_rwc.t_shorts = 0;
   }
   g_rwc.p = bp;
   g_rwc.s = bs;
   g_rwc.level = level;
   fe_host_rewind_set_cadence((uint32_t)bs, !(bp == 1 && bs == 1));
}

/* One display tick's worth of the rewind control.  1 = run a normal frame. */
static int psp_rewind_tick_run(int ff)
{
   fe_host_rewind_info inf;
   int held = g_rw_held, eff, pre, r, was = g_rw_showing;
   uint32_t step = 1, periods = 1;
   unsigned now_us = (unsigned)sceKernelGetSystemTimeLow();
   const char *why;

   if (!g_rw_on)
   {
      g_rwh.denied = g_rw_gb && held ? 2 : 0;
      return 1;
   }
   fe_host_rewind_set_ff(ff);
   if ((why = fe_host_rewind_blocked()) != NULL)
   {
      /* The button says why nothing happens, and that is all it does: 1 a
       * link session, 2 a GB / GBC game, 3 anything else. */
      int code = !strcmp(why, "link") ? 1 : !strcmp(why, "gb") ? 2 : 3;
      if (held && g_rwh.denied != code)
         fe_evt("rewind_denied why=%s", why);
      g_rwh.denied = held ? code : 0;
      g_rwh.intensity = g_rwh.ui = g_rwh.p = 0.0f;
      if (was)
         psp_rewind_show_end();
      return fe_host_rewind_tick(0) == 2;
   }
   g_rwh.denied = 0;
   if (held)
   {
      g_rwh.intensity += 1.0f / 8.0f;
      if (g_rwh.intensity > 1.0f)
         g_rwh.intensity = 1.0f;
   }
   else
   {
      g_rwh.intensity -= 1.0f / 12.0f;
      if (g_rwh.intensity < 0.0f)
         g_rwh.intensity = 0.0f;
   }
   eff = held && g_rwh.intensity >= 1.0f;
   /* half way up: the press is no accident, start getting the frames
    * before ready (a held tick that goes back nothing) */
   pre = g_rwc.on && held && !eff && g_rwh.intensity >= 0.5f;
   if (g_rwc.on)
      fe_host_rewind_set_tick_budget(RWC_PERIOD_US - RWC_SLACK_US);
   if (eff && g_rwc.on)
   {
      int first = g_rwh.held_ticks == 0, level;
      float secs;
      if (!first)
      {
         /* how many display periods the last loop took: a tick that was
          * given several to work in, or one that ran over */
         periods = (now_us - g_rwc.last_us + RWC_PERIOD_US / 2) / RWC_PERIOD_US;
         if (periods < 1) periods = 1;
         if (periods > 6) periods = 6;
      }
      g_rwc.last_us = now_us;
      g_rwh.held_ticks += (int)periods;
      secs = (float)g_rwh.held_ticks / 60.0f;
      level = secs < 1.0f ? 1 : secs < 2.5f ? 2 : 4;
      if (level > g_rwc.speed_max)
         level = g_rwc.speed_max;
      fe_host_rewind_info_get(&inf);
      if (first)
      {
         g_rwc.fail = 0;
         g_rwc.p = g_rwc.s = 0;
         psp_rwc_choose(1, &inf, "start");
         g_rwc.first_p = g_rwc.p;
         g_rwc.first_s = g_rwc.s;
         g_rwc.phase = g_rwc.p;           /* the first step is due now */
      }
      else
      {
         g_rwc.phase += (int)periods;
         if (level != g_rwc.level)
            psp_rwc_choose(level, &inf, "ramp");
      }
      if (g_rwc.phase >= g_rwc.p)
      {
         step = (uint32_t)g_rwc.s;
         g_rwc.phase -= g_rwc.p;
         if (g_rwc.phase >= g_rwc.p)
            g_rwc.phase = g_rwc.p - 1;    /* at most one step owed */
      }
      else
         step = 0;
      /* the tick may work until the next step is due */
      fe_host_rewind_set_tick_budget((uint32_t)(g_rwc.p - g_rwc.phase) *
                                     RWC_PERIOD_US - RWC_SLACK_US);
   }
   else if (eff)
   {
      float secs = (float)(++g_rwh.held_ticks) / 60.0f;
      float speed = 1.0f + (secs - 0.8f) * (3.0f / 2.7f);
      if (speed < 1.0f) speed = 1.0f;
      if (speed > 4.0f) speed = 4.0f;
      g_rwh.acc += speed;
      step = (uint32_t)g_rwh.acc;
      g_rwh.acc -= (float)step;
      if (!step)
         step = 1;
   }
   else
   {
      g_rwh.held_ticks = 0;
      g_rwh.acc = 0.0f;
   }
   r = fe_host_rewind_tick_step(eff || pre, eff ? step : 0u);
   fe_host_rewind_info_get(&inf);
   g_rw_showing = inf.active;
   if (eff && g_rwc.on)
   {
      unsigned sp = (unsigned)g_rwc.s * 1000u / (unsigned)g_rwc.p;
      g_rwc.ticks += periods;
      g_rwc.stepped += inf.stepped;
      g_rwc.t_ticks += periods;
      g_rwc.t_frames += inf.stepped;
      if (step)
      {
         g_rwc.shows++;
         g_rwc.t_shows++;
         if (inf.stepped < step && !inf.at_limit)
         {
            g_rwc.shorts++;
            /* (the first window of a hold is allowed its hiccup) */
            if (g_rwh.held_ticks > 18)
               g_rwc.t_shorts++;
         }
         if (g_rwc.t_ticks >= 48 && g_rwc.t_shorts * 4u > g_rwc.t_shows &&
             !(g_rwc.force_p > 0 && g_rwc.force_s > 0))
         {
            /* It does not hold: this speed is out for the rest of the
             * hold, and no longer remembered as one that held. */
            unsigned got = g_rwc.t_frames * 1150u / g_rwc.t_ticks + 1u;
            g_rwc.fail = got < sp ? got : sp;
            if (g_rwc.held_ok >= g_rwc.fail)
               g_rwc.held_ok = 0;
            g_rwc.demotes++;
            psp_rwc_choose(g_rwc.level, &inf, "short");
         }
         else if (g_rwc.t_ticks >= 90 && g_rwc.t_shorts * 8u <= g_rwc.t_shows)
         {
            if (sp > g_rwc.held_ok)
               g_rwc.held_ok = sp;
            if (g_rwc.t_ticks >= 240)
            {
               g_rwc.t_ticks /= 2;
               g_rwc.t_frames /= 2;
               g_rwc.t_shows /= 2;
               g_rwc.t_shorts /= 2;
            }
         }
      }
      /* the ring turns at the pace kept, not in jumps */
      psp_rewind_hud_step(held, !inf.at_limit, inf.stepped, periods, &inf);
   }
   else
      psp_rewind_hud_step(held, eff && r == 1, inf.stepped, 1, &inf);
   if (was && !g_rw_showing)
   {
      psp_rewind_show_end();
      if (g_rwc.on && g_rwc.ticks)
      {
         fe_evt("rewind_cadence periods=%u show_ticks=%u short=%u frames=%u "
                "demotes=%u tier_changes=%u first=p%ds%d last=p%ds%d "
                "frames_per_period_x100=%u held_x1000=%u", g_rwc.ticks,
                g_rwc.shows, g_rwc.shorts, g_rwc.stepped, g_rwc.demotes,
                g_rwc.changes, g_rwc.first_p, g_rwc.first_s, g_rwc.p, g_rwc.s,
                g_rwc.stepped * 100u / g_rwc.ticks, g_rwc.held_ok);
         /* What a drawn re-run frame spent around the second core: the
          * whole post, of it waiting for the frame before to finish, posts
          * given up because it had not (no picture), and what showing a
          * picture waited for one still being drawn. */
         fe_evt("rewind_me posts=%u post_us=%u retire_wait_us=%u busy=%u "
                "show_waits=%u show_wait_us=%u", g_rwm.posts,
                g_rwm.posts ? g_rwm.post_us / g_rwm.posts : 0,
                g_rwm.posts ? g_rwm.retire_us / g_rwm.posts : 0, g_rwm.busy,
                g_rwm.show_waits,
                g_rwm.show_waits ? g_rwm.show_us / g_rwm.show_waits : 0);
         memset(&g_rwm, 0, sizeof(g_rwm));
      }
      g_rwc.ticks = g_rwc.shows = g_rwc.shorts = g_rwc.stepped = 0;
      g_rwc.demotes = g_rwc.changes = 0;
   }
   if (r != 2)
      return 0;
   /* Not rewinding.  While a press is still rising the frame on screen
    * holds; once it bites and there is nothing to go back to, the game
    * simply plays. */
   return !(held && !eff);
}

/* LAND A REWIND IN PROGRESS before anything else happens to the machine or
 * to its history (docs/REWIND.md 22): the sleep handshake's park point, a
 * state save or load, the menu, the pause overlay, Mystery Gift.  While a
 * rewind is on screen the machine is on some OLD frame, re-running, with the
 * save write-protected; only fe_rewind's landing brings it to a frame play
 * may continue from.  Main thread, between frames.  budget_us 0 = as long as
 * it takes (tens of milliseconds; a second if the frame on screen is inside
 * a save and the landing has to go round it).  Returns 1 when landed. */
/* THE POWER SWITCH WAITS while the machine stands inside an old save
 * sequence (fe_host_rewind_unvetted): the one state in which a sleep could
 * not be made safe within the callback's wait if the landing took too long
 * (the game would have to be reset on the wake).  scePowerLock defers the
 * request, as it does for every other window that cannot park (pwr_hold);
 * it is delivered when the rewind has moved on or landed -- at the latest
 * when the control is let go.  Outside that state nothing is held: the
 * switch works at once, and the park point lands the rewind.  Main thread,
 * every loop and after every rewind tick. */
/* @extract:begin decl:g_rw_pwr */
static int g_rw_pwr;
/* @extract:end decl:g_rw_pwr */
/* @extract:begin fn:psp_rewind_pwr */
static void psp_rewind_pwr(void)
{
   int want = g_rw_on && fe_host_rewind_unvetted();
   if (want == g_rw_pwr)
      return;
   g_rw_pwr = want;
   if (want)
      pwr_hold("rewind");
   else
      pwr_release();
}
/* @extract:end fn:psp_rewind_pwr */

static int psp_rewind_land(const char *why, unsigned budget_us)
{
   int ok;
   (void)why;
   if (!g_rw_on || !fe_host_rewind_busy())
      return 1;
   /* Turbo buttons: a rewind that is landed is over, and so is any auto-
    * fire (frontend state only; nothing of the landing depends on it). */
   ctl_turbo_stop(&g_turbo);
   fe_host_rewind_set_tick_budget(RWC_PERIOD_US - RWC_SLACK_US);
   ok = fe_host_rewind_land(budget_us);
   fe_evt("rewind_land_psp why=%s ok=%d budget_us=%u", why, ok, budget_us);
   /* the control is up as far as the next loop is concerned */
   g_rw_held = 0;
   g_rwh.held_ticks = 0;
   g_rwh.acc = 0.0f;
   g_rwh.intensity = 0.0f;
   g_rwc.ticks = g_rwc.shows = g_rwc.shorts = g_rwc.stepped = 0;
   g_rwc.demotes = g_rwc.changes = 0;
   if (ok && g_rw_showing)
   {
      psp_rewind_show_end();
      g_rw_showing = 0;
   }
   psp_rewind_pwr();
   return ok;
}

static int psp_rewind_tick(int ff)
{
   int r;
   if (g_rw_pool_suspect)
   {
      /* (a blocking loop slept and came back inside this iteration) */
      g_rw_pool_suspect = 0;
      psp_rewind_pool_restart();
   }
   r = psp_rewind_tick_run(ff);
   psp_rewind_pwr();
   return r;
}
#endif /* REWIND */

