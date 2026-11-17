/* ------------------------------------------------------------------ audio */

/* OUT_RATE lives in ff_psp.h: the pacing engine needs it too. */
#define OUT_CHUNK      1024                 /* output frames per blocking write */
/* Power of two, 0.5 s at the core's 32768 Hz (ADR-0028).  The pacer holds
 * the level at AUDIO_RING_TARGET (4096, 125 ms) and a GB link session at
 * its 3072-frame cushion (trim RING_HIGH_WATER + 1024); only an ff_audio
 * fast-forward fills it to the top, and every frame of it is heard as lag
 * once FF ends.  It was 32768 (1.0 s, 128 KiB of .bss) up to 4.6.0-beta.4;
 * 16384 keeps the harness audio_ring_target range up to 16383, room for
 * the "raise the target to 8192 if it gaps" fallback. */
#define RING_FRAMES    16384
#define RING_MASK      (RING_FRAMES - 1)
/* Audio-master pacing nudge (plan §4.4): skip an extra vblank when the ring
 * holds more than the target — bleeds off the 59.94-vs-59.7275 Hz surplus.
 *
 * THE TARGET IS THE AUDIO LATENCY.  The loop is vblank-locked at 59.94 Hz
 * and every frame produces 1/59.7275 s of sound, so the ring gains ~0.36 %
 * (~116 frames a second at 32768 Hz) until the nudge holds it at this level,
 * where it then stays for the rest of the session: sound plays this far
 * behind the picture.  It was RING_FRAMES * 3 / 4 = 24576 = 0.75 s, reached
 * ~3.5 minutes into every session -- the field reports "audio drifts out of
 * sync after minutes, opening the menu resets it" (the menu drains the ring)
 * and "bilinear delays audio" (docs/SA2-TITLE.md).  Hardware logs show the
 * nudge firing 2-3 times per 600 frames from ~200 s on, exactly that.
 *
 * 4096 frames = 125 ms.  The first ~35 s of EVERY session have always run
 * with less than this in the ring (it starts empty and grows at ~116/s), so
 * this is not a smaller cushion than the one play already starts on; it only
 * stops the cushion growing to 0.75 s.  Harness `audio_ring_target`
 * overrides it for A/B arms. */
#ifndef AUDIO_RING_TARGET
#define AUDIO_RING_TARGET 4096
#endif
static unsigned g_ring_high_water = AUDIO_RING_TARGET;
#define RING_HIGH_WATER g_ring_high_water

static int16_t audio_ring[RING_FRAMES * 2];
static volatile unsigned ring_w;            /* producer: emu thread  */
static volatile unsigned ring_r;            /* consumer: audio thread */
/* A link session's connecting / result screen: no game is running, so the
 * ring is emptied once and the producer drops everything until the screen
 * goes (hw2 W: the host's "buggy audio" through the transfer). */
static volatile int g_link_mute;
/* GB link telemetry (EVT gblink_win): netdrv datagrams waiting at a pump. */
static unsigned g_gbl_rxq_max;
static volatile int audio_running;
/* Output frames the audio thread filled with silence because the ring was
 * empty (telemetry only; EVT audio_ring). */
static volatile unsigned g_audio_starved;
/* ...and how many times it ran dry (a gap each; EVT gblink_win). */
static volatile unsigned g_audio_underruns;
/* A GB link session is running (gblink_audio_session): an underrun fades
 * the last sample out over ~1.5 ms and the stream fades back in, instead of
 * stepping to silence and back -- each step is a click, and a CPU-bound
 * session made a stream of them (the owner's "crackly audio during frame
 * drops").  Off outside sessions: single-player output stays exactly as it
 * was. */
static volatile int g_audio_soft;
#define AUDIO_FADE_STEP (65536 / 64)       /* 64 output frames, 1.45 ms */
/* A GB link session's rate follow holds the ring at its cushion and the
 * high-water nudge is off (gblink_audio_frame).  What the follow's +0.5 %
 * cannot drain quickly -- the few frames a console runs flat out to catch
 * its vblank target up after the connecting screen put ~3000 frames in the
 * ring at once in PPSSPP -- is not queued past this level: those samples
 * are dropped (a skip, once) instead of playing 100 ms late for a minute or
 * delaying the frame (a stall for the partner).  0 = no limit. */
static unsigned g_audio_trim;
static volatile int audio_output_error;
/* Resampler input rate.  Seeded with the core's historical default only so
 * the ring is sane before the core exists (audio_start runs before
 * fe_host_boot — see main()); the REAL value is taken from
 * fe_host_sample_rate() right after boot (ADR-0028).  Never hardcode this
 * ratio anywhere else. */
unsigned in_rate = 32768;
static SceUID audio_thid = -1;
/* The worker's sceAudio channel while it holds one.  It gives the channel
 * back on its way out; a worker terminated in its output (audio_stop) never
 * gets there, so the stop does, or each restart would take another of the
 * eight until the game went silent. */
/* @extract:begin decl:audio_ch */
static volatile int audio_ch = -1;
/* @extract:end decl:audio_ch */

/* Resampler phase increment, 16.16, read fresh by the audio thread on every
 * output chunk.  Normally (in_rate << 16) / OUT_RATE.  ADR-0027 lowers it in
 * proportion to the pace target: when the emulator deliberately runs at, say,
 * 46.5 of the GBA's 59.7275 fps it also produces audio 46.5/59.7275 as fast,
 * so a consumer running at the nominal rate would drain the ring and emit
 * silence for ~22 % of every second.  Following the pace keeps the stream
 * CONTINUOUS at the cost of a proportional pitch drop — see ADR-0027 §audio
 * for why that beat the alternative. */
volatile unsigned g_audio_step;

/* Audio correctness oracle.  The frame-dump oracle proves the VIDEO output of
 * a dynarec change is unaltered, and says nothing at all about sound — which
 * matters because GBA sound engines (M4A/Sappy) classically live in IWRAM and
 * SELF-MODIFY, patching per-channel volume/pitch immediates.  That is exactly
 * the code partial invalidation retires, so audio is the likeliest place for
 * a stale-jump bug to surface.  Rolling hash over every sample the core
 * produces, logged at exit; deterministic for a scripted run, so it compares
 * across builds the same way the frame hash does. */
static uint32_t g_audio_hash = 2166136261u;
static uint32_t g_audio_samples;
static int g_audio_oracle;

uint32_t plat_audio_hash(void)    { return g_audio_hash; }
uint32_t plat_audio_sample_count(void) { return g_audio_samples; }

static void plat_audio_frames(const int16_t *lr, size_t frames)
{
   size_t i;
   /* Hash BEFORE the FF/ring early-outs: this must describe what the core
    * generated, not what the output path happened to accept.  Bench-only —
    * The dedicated perf build selects it with audio_oracle; older developer
    * builds retain bench_mode. It is pure instrumentation and must stay off
    * in scoring runs and absent from a shipping build. */
#ifdef GPSP_PERF_RIG
   if (g_audio_oracle)
#else
   if (g_pcfg.bench_mode)
#endif
   {
      for (i = 0; i < frames * 2; i++)
         g_audio_hash = (g_audio_hash ^ (uint16_t)lr[i]) * 16777619u;
      g_audio_samples += (uint32_t)frames;
   }

   if ((g_ff_uncapped || g_ff_mult) && !g_pcfg.ff_audio)
      return;   /* FF mutes audio: ring drains to silence */
   if (g_link_mute)
      return;   /* link screen: silence */
   if (g_audio_trim && ring_w - ring_r > g_audio_trim)
      return;   /* link session: over the latency bound, see g_audio_trim */
   /* The worker has stopped (output error, see audio_thread): nothing drains
    * the ring any more, and a ring held above RING_HIGH_WATER makes the pacer
    * add a vblank to EVERY frame -- a silent game running at half speed. */
   if (!audio_running)
      return;
   for (i = 0; i < frames; i++)
   {
      if (ring_w - ring_r >= RING_FRAMES)
         break;                              /* full: drop the tail */
      {
         unsigned idx = ring_w & RING_MASK;
         audio_ring[idx * 2 + 0] = lr[i * 2 + 0];
         audio_ring[idx * 2 + 1] = lr[i * 2 + 1];
         RING_BARRIER();
         ring_w++;
      }
   }
}

/* Feed the core's audio-buffer-status hook once per main-loop iteration
 * (ADR-0018, rescoped by ADR-0019).  It is a no-op unless the core is
 * actually running auto/auto_threshold frameskip — which, since ADR-0019,
 * a session no longer turns on by default: the field showed both consoles
 * holding real time (58.9 fps emulated) while `auto` quietly dropped
 * rendered frames on a transient buffer dip.  `active` is false while FF
 * mutes audio, so FF keeps its own policy. */
#define AUDIO_UNDERRUN_PCT 25

static void audio_status_frame(void)
{
   unsigned level = ring_w - ring_r;
   /* Occupancy of the INTENDED cushion (RING_HIGH_WATER), not of the whole
    * ring: the pacer holds the level at the target, so measured against
    * RING_FRAMES a healthy ring would read 12.5 % and `auto` would call it
    * an underrun forever.  With the old 3/4 target this read 0-75 %. */
   int pct = (int)((uint64_t)level * 100u / RING_HIGH_WATER);
   int active = audio_running &&
                (g_pcfg.ff_audio || (!g_ff_uncapped && !g_ff_mult));
   if (pct > 100)
      pct = 100;
   fe_host_audio_buffer_status(active, pct, pct < AUDIO_UNDERRUN_PCT);
}

/* EVT audio_ring: the ring level as the pacer sees it, once per paced frame,
 * reported every 600 frames.  There was no ring-level observable at all
 * outside link sessions (frame_hist's `nudge` is session-only), which is how
 * a 0.75 s audio lag went unseen.  lvl/min/max in ring frames (in_rate
 * samples), starved = output frames the audio thread filled with silence. */
#ifdef GPSP_KEEP_TELEMETRY
/* The audio ring's telemetry (code-health C5-2).  The window's minimum keeps its own
 * object: it starts at ~0u, and a struct member with an initializer would move the
 * whole struct from .bss to .data. */
static struct fe_audio_tel
{
   unsigned n, max, nudge, starved0;
} audio_tel;
static unsigned g_ar_min = ~0u;
#endif

/* A telemetry build's window line; a player build's call is empty. */
static void audio_ring_note(void)
{
#ifdef GPSP_KEEP_TELEMETRY
   unsigned level = ring_w - ring_r;
   if (level < g_ar_min)
      g_ar_min = level;
   if (level > audio_tel.max)
      audio_tel.max = level;
   if (++audio_tel.n >= 600)
   {
      unsigned st = g_audio_starved;
      fe_evt("audio_ring n=%u lvl=%u min=%u max=%u target=%u nudge=%u "
             "starved=%u in_rate=%u", audio_tel.n, level, g_ar_min, audio_tel.max,
             g_ring_high_water, audio_tel.nudge, st - audio_tel.starved0, in_rate);
      audio_tel.starved0 = st;
      audio_tel.n = 0;
      g_ar_min = ~0u;
      audio_tel.max = 0;
      audio_tel.nudge = 0;
   }
#endif
}

_Static_assert(PCFG_AUDIO_HW_MAX == PSP_AUDIO_VOLUME_MAX,
               "Settings > Volume at 100 must be the channel's full volume");

/* Settings > Volume as the channel volume sceAudioOutputBlocking takes
 * (config_psp.h VOLUME).  Precomputed by psp_audio_volume_apply (at boot,
 * before audio_start, and when the row changes) so audio_thread -- in the
 * pinned hot order (psp/layout/hot.ord) -- only loads a word where it had
 * an immediate: one instruction, not the clamp and the divide. */
static volatile int g_audio_hw_vol;

void psp_audio_volume_apply(void)
{
   g_audio_hw_vol = pcfg_audio_hw_volume(g_pcfg.volume);
}

/* @extract:begin fn:audio_thread */
static int audio_thread(SceSize args, void *argp)
{
   static int16_t out[OUT_CHUNK * 2];
   unsigned step;
   unsigned frac = 0;
   int ch;
   int last_l = 0, last_r = 0;      /* g_audio_soft: the fade's start */
   int fade = 65536;                /* 16.16 gain while fading in/out */
   int dry = 0;                     /* the ring is empty (one gap) */

   (void)args; (void)argp;

   ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_CHUNK,
                          PSP_AUDIO_FORMAT_STEREO);
   if (ch < 0)
   {
      audio_output_error = ch;
      audio_running = 0;
      return 0;
   }
   audio_ch = ch;

   while (audio_running)
   {
      int i;
      /* ADR-0027: re-read once per chunk (~23 ms), not per sample — the pace
       * target moves at most once a second and a chunk boundary is the
       * natural place to take the step change. */
      step = g_audio_step;
      /* A flush (`ring_r = ring_w`) from a thread that outranks this one,
       * landing between the `avail` check and `ring_r +=` below, would leave
       * ring_r AHEAD of ring_w: the level then reads ~4 G for good, the
       * producer drops everything and the pacer's high-water nudge halves the
       * frame rate.  Resynchronise.  The power callback's me_standby_down()
       * was that flush; it now runs only after the sleep quiesce has stopped
       * this thread (sq_audio_stop), so this is a guard, not a fix. */
      if (ring_w - ring_r > RING_FRAMES)
         ring_r = ring_w;
      for (i = 0; i < OUT_CHUNK; i++)
      {
         unsigned avail = ring_w - ring_r;
         if (avail < 2)
         {
            if (!dry)
            {
               dry = 1;
               g_audio_underruns++;          /* EVT gblink_win under= */
            }
            if (g_audio_soft && fade > 0)
            {
               /* the last sample, fading to silence: no step, no click */
               fade = fade > AUDIO_FADE_STEP ? fade - AUDIO_FADE_STEP : 0;
               out[i * 2 + 0] = (int16_t)((last_l * fade) >> 16);
               out[i * 2 + 1] = (int16_t)((last_r * fade) >> 16);
            }
            else
            {
               out[i * 2 + 0] = 0;           /* starved: silence */
               out[i * 2 + 1] = 0;
               fade = 0;
            }
            g_audio_starved++;               /* EVT audio_ring starved= */
            continue;
         }
         RING_BARRIER();
         {
            unsigned i0 = ring_r & RING_MASK;
            unsigned i1 = (ring_r + 1) & RING_MASK;
            int l0 = audio_ring[i0 * 2 + 0], l1 = audio_ring[i1 * 2 + 0];
            int r0 = audio_ring[i0 * 2 + 1], r1 = audio_ring[i1 * 2 + 1];
            int f = (int)(frac & 0xFFFF);
            /* 64-bit product: |l1 - l0| reaches 65535 and f 65535, which
             * overflows int (undefined behaviour; a click on this target). */
            int sl = l0 + (int)(((int64_t)(l1 - l0) * f) >> 16);
            int sr = r0 + (int)(((int64_t)(r1 - r0) * f) >> 16);
            dry = 0;
            if (g_audio_soft)
            {
               if (fade < 65536)
               {
                  /* back from a gap: fade in rather than jump */
                  fade = fade + AUDIO_FADE_STEP < 65536
                            ? fade + AUDIO_FADE_STEP : 65536;
                  sl = (sl * fade) >> 16;
                  sr = (sr * fade) >> 16;
               }
               last_l = sl;
               last_r = sr;
            }
            else
               fade = 65536;
            out[i * 2 + 0] = (int16_t)sl;
            out[i * 2 + 1] = (int16_t)sr;
         }
         frac += step;
         RING_BARRIER();
         ring_r += frac >> 16;
         frac &= 0xFFFF;
      }
      {
         /* Settings > Volume as the channel's own volume: no sample is
          * scaled, and at 100 this is PSP_AUDIO_VOLUME_MAX as it was. */
         int rc = sceAudioOutputBlocking(ch, g_audio_hw_vol, out);
         if (rc < 0)
         {
            /* A failed blocking write may return immediately.  This thread
             * outranks emulation, so retrying would starve the main loop. */
            audio_output_error = rc;
            audio_running = 0;
            break;
         }
      }
   }

   /* Cleared first: a stop that terminates us from here on finds nothing
    * left to release. */
   audio_ch = -1;
   sceAudioChRelease(ch);
   return 0;
}
/* @extract:end fn:audio_thread */

/* @extract:begin fn:audio_start */
static void audio_start(void)
{
   /* Started once (r1-B5): a second start would leak the running worker,
    * which still owns its channel. */
   if (audio_thid >= 0)
      return;
   audio_output_error = 0;
   audio_running = 1;
   /* Nominal until the pace policy says otherwise (ADR-0027). */
   g_audio_step = (unsigned)(((uint64_t)in_rate << 16) / OUT_RATE);
   /* 8 KiB stack (was 16).  audio_thread is a leaf loop: its frame is 64 B
    * (release ELF: addiu sp,sp,-64; `out` is static), and its only calls
    * are sceAudioChReserve / OutputBlocking / ChRelease, syscalls that run
    * on the kernel stack.  The stack does NOT come from the heap: newlib's
    * heap is one partition block taken before main (PSP_HEAP_SIZE_KB(-1024),
    * docs/MEMORY-BUDGET.md), so thread stacks come out of the 1 MiB left
    * outside it, which sceUtility and the net modules share.  The 8 KiB is
    * headroom there, not ROM cache. */
   audio_thid = sceKernelCreateThread("gpsp_audio", audio_thread, 0x12,
                                      0x2000, THREAD_ATTR_USER, NULL);
   if (audio_thid >= 0)
   {
      int rc = sceKernelStartThread(audio_thid, 0, NULL);
      if (rc < 0)
      {
         audio_output_error = rc;
         audio_running = 0;
         sceKernelDeleteThread(audio_thid);
         audio_thid = -1;
      }
   }
   else
   {
      audio_output_error = audio_thid;
      audio_running = 0;
   }
}
/* @extract:end fn:audio_start */

static void audio_ring_reset(void)
{
   ring_w = ring_r = 0;
   g_audio_hash = 2166136261u;
   g_audio_samples = 0;
   in_rate = 32768;
}

/* Returns 0 = joined (the worker released its channel on the way out),
 * 1 = there was no worker, < 0 = the join timed out and it was terminated
 * (its channel released here). */
/* @extract:begin def:AUDIO_JOIN_US */
#define AUDIO_JOIN_US 1000000u
/* @extract:end def:AUDIO_JOIN_US */
/* @extract:begin fn:audio_stop */
static int audio_stop(void)
{
   int rc = 1;
   audio_running = 0;
   if (audio_thid >= 0)
   {
      /* Bounded: one chunk is ~23 ms, so a worker that has not returned from
       * sceAudioOutputBlocking in a second never will -- do not hang exit. */
      SceUInt tmo = AUDIO_JOIN_US;
      rc = sceKernelWaitThreadEnd(audio_thid, &tmo);
      if (rc < 0)
      {
         sceKernelTerminateDeleteThread(audio_thid);
         /* Terminated in its output, it still holds its channel. */
         if (audio_ch >= 0)
         {
            sceAudioChRelease(audio_ch);
            audio_ch = -1;
         }
      }
      else
      {
         sceKernelDeleteThread(audio_thid);
         rc = 0;
      }
      audio_thid = -1;
   }
   return rc;
}
/* @extract:end fn:audio_stop */

/* Report audio-device failures from the main thread, never the high-priority
 * audio worker.  Emulation can continue silently after an output API error. */
static void audio_report_error(void)
{
   int rc = audio_output_error;
   if (rc)
   {
      audio_output_error = 0;
      fe_evt("audio output disabled rc=%d", rc);
      osd_toast("Audio output unavailable");
   }
}

