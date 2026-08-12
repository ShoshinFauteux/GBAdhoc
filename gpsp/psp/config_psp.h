/* config_psp.h — persisted frontend settings (plan §8), ini-backed
 * (<appdir>/config.ini).  Loaded at boot, saved on change.  Distinct from
 * .gpsp-harness.ini (harness channel, ADR-0036) and variant.ini
 * (per-game builds). */
#ifndef CONFIG_PSP_H
#define CONFIG_PSP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ADR-0071 build-scoped defaults.  A PLAYABLE build ships an upscaled picture
 * and the speed profile; the HARNESS keeps 1x so its frame-time numbers stay
 * comparable with every run recorded before this existed. */
#define PCFG_PROFILE_SPEED   0
#define PCFG_PROFILE_COMPAT  1
/* ADR-0073: THE SPEED PROFILE IS 57.00, NOT FULL SPEED, AND THAT IS THE POINT.
 *
 * Emerald stops reading the CLIENT's buttons whenever its RFU receive queue is
 * more than 4 deep (pokeemerald src/overworld.c:2520, KeyInterCB_SelfIdle) and
 * that queue drains exactly once per frame.  It exists only on the child, so
 * the host structurally cannot suffer it.  A host that produces packets faster
 * than the client consumes them therefore fills it and never empties it.
 *
 * At 59.73 the host reaches ~59.4 while the client manages ~56.6 -- and
 * net_pace_match is a CEILING, so nothing reconciles them.  Measured over 100+
 * scripted runs on two consoles:
 *
 *   target   trades        client fails to reach its seat
 *   59.73    21/45 = 0.47  17/45
 *   55.00    15/25 = 0.60   5/25
 *   57.00    13/13 = 1.00   0/13
 *
 * 57.00 is the highest tested rate the client still tracks closely (-0.7 fps
 * vs -2.0 at full speed), which keeps frames short -- delivery clumping rises
 * as the rate falls, so lower is not automatically safer and 53.00 was worse
 * than 55.00 on every measure.
 *
 * THIS ONLY APPLIES WHILE A WIRELESS SESSION IS LIVE.  Single-player is
 * unpaced and still runs as fast as the console can manage. */
#define PCFG_PROFILE_FPS_X100(p)  ((p) == PCFG_PROFILE_COMPAT ? 2997 : 5700)
#ifdef GPSP_PLAYABLE
/* VID_SCALE_FIT (1): fills the 272-px height at the correct 3:2 aspect,
 * 408x272.  NOT stretch — stretch is 480x272, which is 18 % more GE fill AND
 * distorts, and it is one press of the video preset in the menu if wanted. */
#define PCFG_SCALE_DEF       1
/* Media Engine mode ON by default for players (2026-08-10): the second-core
 * renderer ran five consecutive flawless harness runs (~40k frames, 0 drops),
 * fps floor above the CPU path, user-validated visuals — and it self-disables
 * to CPU rendering if the ME fails its boot handshake or wedges, so a model
 * where it misbehaves degrades instead of breaking.  The settings toggle
 * ("Media Engine mode", restart-to-apply) turns it off. */
#define PCFG_ME_MODE_DEF     1
#else
#define PCFG_SCALE_DEF       0
#define PCFG_ME_MODE_DEF     0   /* harness: arms opt in explicitly */
#endif

typedef struct
{
   int  scale;        /* VID_SCALE_* (default 1x) */
   int  filter;       /* VID_FILTER_* (default nearest) */
   /* ADR-0071: TRADING PROFILE.  0 = speed (DEFAULT), 1 = compatibility.
    *
    * Only the playable/variant builds consult this; the harness sets its rate
    * explicitly and ignores it.  Changing it requires a RELAUNCH, because the
    * session rate is latched into the pacer and the netdrv timeout scaling at
    * startup and half-changing it mid-session would leave the two consoles
    * disagreeing about how long a frame is — which is precisely the failure
    * gen-3's RFU cannot survive (it counts link timeouts in FRAMES).
    *
    *   0 speed          59.73 fps.  Full speed, and 1.00x the adapter timeout
    *                    budget a real GBA gets — exactly faithful.
    *   1 compatibility  29.97 fps.  Half rate, and therefore 1.99x that
    *                    budget.  The extra wall-clock per emulated timeout is
    *                    the actual mechanism, not "less work per second". */
   int  profile;
   /* Media Engine mode (user opt-in, default 0): run the GBA scanline
    * renderer on the PSP's second CPU core.  Applied at boot (the ME PRX,
    * capture mode and staging buffers are boot-time constructions), so the
    * settings UI relaunches the app when it changes — the same ADR-0071
    * restart flow the trading profile used. */
   int  me_mode;
   /* GBA A/B face-button layout.  0 (default) = Circle is A, Cross is B —
    * the positional match to a real GBA.  1 = swapped: Cross is A, Circle
    * is B, for players who want the western confirm button as A. */
   int  btn_swap;
   int  ff_mult_x10;  /* 15 / 30 / 0 = uncapped (default 15; 20 retired) */
   int  ff_hold;      /* 1 = hold (default), 0 = toggle */
   int  theme;        /* UI palette: 0 = dark (default), 1 = light */
   int  show_fps;     /* 1 = OSD chip with emulated-frame rate (default 0) */
   /* Wireless-session frameskip policy (ADR-0019).  0 = off (default, the
    * smooth one), 1 = adaptive (auto_threshold with hysteresis, engaged
    * only after sustained real-time slippage), 2 = auto (ADR-0018's
    * blanket behaviour, kept for hardware A/B). */
   int  net_frameskip;
   /* gpsp_frameskip_threshold used when the adaptive policy engages, as the
    * core's option string (audio-buffer occupancy %, core default 33). */
   char net_skip_threshold_str[8];
   /* TX offload thread (ADR-0021).  0 = inline (pre-ADR-0021 behaviour),
    * 1 = prompt (default, TX thread above main), 2 = deferred (TX thread
    * below main, sends drain in the vblank slack).  All three kept so the
    * user can A/B them on hardware in one sitting without a rebuild. */
   int  net_tx_thread;
   /* EVT/LOG writer thread (ADR-0024).  0 = inline fprintf+fflush on the
    * emulation thread (pre-ADR-0024 behaviour), 1 = default: lines go to a
    * ring and a low-priority writer thread does the memory-stick I/O in the
    * slack the emulation thread already donates at the vblank wait.  The
    * field measured 12.0-12.5 ms inside one flush, on both consoles, inside
    * a 16.7 ms frame.  Accepts either `log_thread` or `net_log_thread`. */
   int  log_thread;
   /* .sav block writer (ADR-0025).  0 = ADR-0020's budgeted drain on the
    * emulation thread, 1 = default: the same writer thread that owns the
    * EVT log does every open/seek/write/flush/close.  The field measured
    * `sram_flush ... ms=29.296` in the one window whose `frame` maximum
    * jumped 37264 -> 63772 us, which is the largest emu-thread stall left
    * after ADR-0024. */
   int  sram_thread;
   /* Session pacing mode.  **THE MEANING OF `1` CHANGED IN ADR-0033** — it
    * used to select ADR-0027/0028's adaptive peer matcher and now selects
    * fixed-rate clamping.  An existing config.ini carrying `net_pace_match=1`
    * therefore gets the NEW behaviour silently, which is deliberate: the
    * fixed clamp is what we want shipped, and the value that used to mean
    * "on" still means "on".
    *   0 = off      — both consoles free-run (unchanged meaning).
    *   1 = fixed    — DEFAULT.  While a session is live both consoles clamp
    *                  to `net_session_fps` (below).  Identical on both sides,
    *                  no negotiation, no control loop, no hunting.
    *   2 = adaptive — ADR-0027/0028's peer-capability matcher, kept verbatim
    *                  so the two policies can be A/B'd on hardware.
    * Gen-3's RFU counts link timeouts in FRAMES, so two emulators 20 % apart
    * (the field's 58 fps host vs 46 fps join) are mutually inconsistent in a
    * way two real GBAs never are.  Equality is what the games require; ADR-0033
    * buys it with a constant instead of a control loop. */
   int  net_pace_match;
   /* Requested fixed session frame rate in hundredths (ADR-0033),
    * `net_session_fps`.  Default 2997 = 29.97 fps = exactly two PSP vblanks
    * (ADR-0035 — 40.00 was unreachable in the field, see PCFG_SESSION_FPS_DEF).
    * This is what the USER ASKED FOR; what is applied is this snapped to an
    * achievable rate.  Only consulted when net_pace_match == 1. */
   int  net_session_fps_x100;
   /* `net_session_fps_snap` — 1 = default: snap the request to the nearest
    * whole-vblank rate (59.94/N), the only rates a console holds without
    * needing spare headroom inside a vblank.  0 = apply the request verbatim
    * via the fractional accumulator, which averages correctly ONLY if frames
    * genuinely fit in one vblank.  Exists so the field can A/B "steady 29.97"
    * against "nominally 40, actually 35-38 with jitter" — that comparison is
    * the reason ADR-0035 exists and it should stay possible. */
   int  net_session_fps_snap;
   /* Per-phase frame bracket (phase 5h, `EVT core_phase`).  0 = off,
    * 1 = coarse (~10 clock reads/frame), 2 = default: + per-scanline video,
    * GBC mixing and dynarec compilation (~350 reads/frame), 3 = + the
    * direct-sound FIFO drain (~1250 reads/frame).  A clock read is ~1 us on
    * Allegrex, so the level IS the probe budget; 1 vs 2 is the A/B that
    * prices the probe by measurement instead of by assertion. */
   int  core_phase;
   /* Staging-buffer placement for the GU blit (ADR-0034, VID_BLIT_*).
    * 0 = cached RAM + an 82 KiB cache writeback every frame (the original
    * path), 1 = the same RAM through the uncached mirror, **2 = VRAM above
    * the display buffers — the DEFAULT, decided by hardware**:
    *
    *              stage    gu    tot   (PSP-1000, us/frame)
    *   0 cached    1337   1176   2513
    *   1 uncached  1041   1174   2215
    *   2 vram      1071    728   1800
    *
    * VRAM saves 713 us/frame against cached, and — against the prediction —
    * most of that is `gu`, because the GE reads its source texture out of
    * VRAM far faster than out of main RAM.  0 and 1 are kept for the A/B. */
   int  blit_mode;
   /* ADR-0040 `gu_defer`: 0 block on the GE inside the blit (historical,
    * DEFAULT), 1 let the list run and sync just before the swap, after the
    * vblank wait has already absorbed most of it.  Worth the most during a
    * clamped session, where the loop idles ~9 ms; A/B it there, not solo. */
   int  gu_defer;
   /* ADR-0042 `rfu_rx_cap`: most RFU packets a JOINING console's emulated
    * adapter will hand the game in one frame.  0 = unlimited = historical
    * behaviour and the DEFAULT.  The field crash correlates with bunched
    * delivery (6/frame vs the rig's 2-3) and the game's receive queue
    * latching full is an unrecoverable error, so 2-3 is the interesting
    * setting.  Held packets wait one frame in our own queue; they are never
    * dropped.  No effect on a hosting console. */
   int  rfu_rx_cap;
   /* Session overlay chip (bottom-right "linked/hosting" status) while a
    * wireless session is live.  1 = shown (default), 0 = hidden.  The
    * WLAN-off warning chip is NOT gated by this: hiding the reason wireless
    * is broken costs a field-debug round trip. */
   int  osd_wireless;
   char group[9];     /* wireless room code (adhocctl group, <= 8 chars) */
   char nick[24];     /* wireless nickname */
   char last_rom[64]; /* browser preselect */
} psp_config;

/* Bounds for `net_session_fps` (ADR-0033), in hundredths of a frame/second.
 * The ceiling is the GBA's own 59.7275 Hz — clamping ABOVE nominal is not a
 * clamp.  The floor is where the audio pitch drop (which follows the pace by
 * design, ADR-0027 §audio) stops being a tape slowdown and starts being a
 * different instrument; a session that needs less than 20 fps has a problem
 * pacing cannot fix.
 *
 * **THE DEFAULT IS 29.97, NOT 40.00 (ADR-0035).**  We pace by inserting whole
 * PSP vblanks, so the only rates a console can hold REGARDLESS of per-frame
 * work are 59.94/N.  The field measured 35-38 fps achieved against a 40.00
 * target: 40 needs about half the frames to fit inside one 16.68 ms vblank,
 * and when per-frame work sits just above that, none do.  29.97 = exactly two
 * vblanks, which needs no headroom at all.  Requests are SNAPPED to the
 * nearest achievable rate and the snap is logged — see pace_snap_x100(). */
/* SHIPPING DEFAULT: 29.97. Do not raise this without oracle-verified runs.
 *
 * 45.00 was promoted here on 2026-08-07 on the strength of 8/8 trades, and a
 * second night falsified it: 12/16 across both nights (0.75), with runs dying
 * on the frozen-client signature. The claim this comment used to make -- "a 50%
 * speed-up at no measured cost in reliability" -- was FALSE, and was written
 * from a single night's sample.
 *
 * Trade success is a SHOULDER, not a cliff (Logs/CLIFF-FINDINGS.md):
 *
 *     29.97 -> 1.00 (16/16)    45.00 -> 0.75 (12/16)    47.00 -> 0.50 (4/8)
 *     49.00 -> 0.00 (0/8)      53.00 -> 0.00 (0/8)      59.73 -> 0.00 (0/16)
 *
 * Only 29.97 is bulletproof. Everything above it is a reliability trade, so the
 * shipping default must be the reliable one until the mechanism is understood.
 *
 * Best current explanation (well-motivated, NOT proven): every adapter timeout
 * is counted in EMULATED CPU CYCLES (rfu.c:1087 -> main.c:844) while the radio
 * is wall clock, so what the game experiences is link latency measured in
 * EMULATED FRAMES -- 1.05 / 2.48 / 2.63 / 2.92 / 3.69 / 23.85 for the rates
 * above, which orders every outcome correctly. It does not yet predict
 * individual runs. */
#define PCFG_SESSION_FPS_DEF  2997
#define PCFG_SESSION_FPS_MIN  2000
#define PCFG_SESSION_FPS_MAX  5973

extern psp_config g_pcfg;

/* Reads a decimal frame rate ("40", "40.5", "40.00") from `ini` as hundredths,
 * clamped to PCFG_SESSION_FPS_MIN..MAX.  Returns `def` when the key is absent
 * or unparseable.  Shared with the harness override in main_psp.c so the
 * harness and config.ini cannot disagree about what "40.00" means. */
int  pcfg_fps_x100(const char *ini, const char *key, int def);

void pcfg_load(const char *ini_path);
void pcfg_save(void);   /* rewrites all keys to the load path */

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PSP_H */
