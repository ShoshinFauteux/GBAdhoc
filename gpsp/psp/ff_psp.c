/* ff_psp.c -- the fast-forward and session-pacing engine.
 *
 * MOVED VERBATIM out of psp/main_psp.c, which was 7460 lines holding this, the
 * Media Engine renderer, wireless session lifecycle, Mystery Gift, suspend and
 * resume, the ROM browser and the main loop.  Changing a fast-forward policy
 * meant reading all of it.  Nothing here was rewritten in the move; see
 * ff_psp.h for the interface and for the host state it still reads.
 *
 * WHAT THIS OWNS
 *   the frameskip policy   (ADR-0019, supersedes ADR-0018)
 *   session pacing         (ADR-0033 fixed-rate; ADR-0027/0028 adaptive)
 * and the 23 state variables behind them, of which 18 are now private to this
 * file.  The five the rest of the frontend reports on are reachable only
 * through the accessors at the bottom.
 *
 * WHAT IT DOES NOT OWN, deliberately: g_vc_target, the absolute vblank the next
 * frame is presented on.  That belongs to the present loop in main_psp.c and
 * this engine never touches it.
 */
#include "ff_psp.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

#include "fe_evt.h"
#include "fe_host.h"
#include "fe_util.h"
#include "config_psp.h"
#include "netpacket_host.h"   /* fe_np_* : peer fps, txq depth */
#include "osd_psp.h"
#include "ui_psp.h"

/* ---- pacing state (was declared beside sess_cost in main_psp.c) --------- */
static int      g_pace_engaged;
static unsigned g_pace_cap_x100;      /* OUR capability (EMA), not achieved */
static unsigned g_pace_peer_x100;     /* last peer capability we acted on */
/* Decaying low-water marks (ADR-0028): the sustained WORST of each side. */
static unsigned g_pace_self_lo, g_pace_peer_lo;
static uint64_t g_pace_self_lo_us, g_pace_peer_lo_us;
static unsigned g_pace_target_x100;   /* applied target, ramped */
static unsigned g_pace_goal_x100;     /* what the target is ramping toward */
static unsigned g_pace_acc;           /* fractional-vblank accumulator, /10000 */
static uint64_t g_pace_win_us;
static unsigned g_pace_win_frames;
static uint64_t g_pace_win_vb;        /* vblank periods this window's work needed */
static uint64_t g_pace_log_us;
static int      g_pace_floor_said;
static int      g_pace_slow_us;       /* harness only: see `pace_slow_us` */
/* ADR-0033 fixed-rate mode. */
static unsigned g_pace_fixed_x100;    /* the clamp: config net_session_fps */
static uint64_t g_pace_ramp_us;       /* last time the glide advanced */
static int      g_pace_miss_said;     /* one session_pace_miss per episode */

/* ---- session frameskip policy (ADR-0019, supersedes ADR-0018) -----------
 *
 * ADR-0018 set gpsp_frameskip=auto for the whole session on the theory that
 * a console falling behind stretches the emulated link's timeouts.  The
 * first successful field session disproved the premise for both consoles:
 * the heartbeat ladder held ~58.9 fps emulated from start to finish, i.e.
 * identical to solo play, so `auto` was throwing away rendered frames while
 * the emulator was already keeping real time.  Worse, `auto` triggers on a
 * transient audio-buffer dip (fe_host_audio_buffer_status < 25 %) which our
 * vblank-locked main loop can produce with plenty of CPU headroom — and
 * because the loop then still waits for vblank, the skip buys back no
 * emulation throughput at all.  Pure user-visible stutter.
 *
 * Policy (config.ini `net_frameskip`):
 *   0 off       DEFAULT — never touch frameskip for a session.
 *   1 adaptive  start disabled; engage a BOUNDED skip only after
 *               SKIP_SLOW_WINDOWS consecutive 1 s windows below
 *               SKIP_SLOW_PCT_TGT of the applied pace target, and drop it
 *               again after SKIP_FAST_WINDOWS windows back above
 *               SKIP_FAST_PCT_TGT of it (ADR-0037 made both relative).
 *               Hysteresis, deliberately: `auto` reacted per frame.
 *   2 auto      ADR-0018 behaviour verbatim, kept so the two can be
 *               compared on real hardware in one sitting.
 * Every transition is logged, so a field log says which policy ran.
 *
 * ADR-0021 amendment — what mode 1 engages.  On hardware it skipped
 * 580 of 600 frames (`EVT fps ... rendered=1.9`) and STILL did not reach
 * 60 fps.  That is not a tuning miss, it is the mechanism: `auto_threshold`
 * skips whenever audio-buffer occupancy is below the threshold, and a
 * console that is genuinely behind never refills the ring, so the only
 * thing bounding it is the core's FRAMESKIP_MAX = 30 consecutive skips
 * (libretro/libretro.c:96,1361-1377) — 30 skipped, 1 rendered, forever.
 * A console short of ~1.4x needs half its renders back, not 97 % of them.
 * So the adaptive mode now engages `fixed_interval` with interval 1: skip
 * one, render one, hard bound, no audio-occupancy feedback loop at all.
 * The hysteresis around it is unchanged.
 *
 * ADR-0037 amendment — "behind" IS RELATIVE TO THE APPLIED PACE TARGET.
 * The two thresholds used to be absolute fps constants (5700/5850) derived
 * from the GBA's nominal 59.7275.  That was right while the only rate we
 * ever aimed at WAS the nominal one.  ADR-0033 then made a session clamp
 * itself to `net_session_fps`, and the constants did not follow: a console
 * pacing perfectly at its 40.00 (or 29.97) target measures 40.00 < 57.00,
 * concludes it is behind on three consecutive windows, and engages
 * interval-1 skip FOR THE WHOLE SESSION.  The field log is unambiguous —
 * `EVT fps emu=29.43 rendered=14.71 skipped=300` with `self_cap` reporting
 * 52-57 fps of capability sitting unused.  The emulator was hitting its
 * target exactly and throwing away half its frames for it.
 *
 * So the thresholds are now a FRACTION of `g_pace_target_x100`, the applied
 * (ramped) target.  Off-session that is PACE_NOMINAL_X100 and the numbers
 * come out at 5697/5853 — within 3/100 fps of the old constants, so this is
 * a no-op everywhere except the case it fixes.  Percentages rather than the
 * old literals because the target moves during the ~5 s glide and a fixed
 * offset would mean something different at each point on the ramp. */
#define SKIP_WIN_US        1000000ull
#define SKIP_SLOW_PCT_TGT     954    /* x10: below 95.4 % of target = behind */
#define SKIP_FAST_PCT_TGT     980    /* x10: above 98.0 % of target = fine   */
#define SKIP_SLOW_WINDOWS      3
#define SKIP_FAST_WINDOWS      5

enum { SKIP_POL_OFF = 0, SKIP_POL_ADAPTIVE, SKIP_POL_AUTO };

static int      g_skip_engaged;      /* auto_threshold currently set */
static int      g_skip_slow, g_skip_fast;
static uint64_t g_skip_win_us;
static unsigned g_skip_win_frames;

void skip_policy_set(const char *mode)
{
   fe_host_option_set_live("gpsp_frameskip", mode);
}

void skip_policy_session_begin(void)
{
   g_skip_engaged = 0;
   g_skip_slow = g_skip_fast = 0;
   g_skip_win_us = net_now_us();
   g_skip_win_frames = fe_host_frame_count();
   if (g_pcfg.net_frameskip == SKIP_POL_AUTO)
   {
      g_skip_engaged = 1;
      skip_policy_set("auto");
   }
   fe_evt("skip_policy mode=%s engaged=%d",
          g_pcfg.net_frameskip == SKIP_POL_AUTO     ? "auto" :
          g_pcfg.net_frameskip == SKIP_POL_ADAPTIVE ? "adaptive" : "off",
          g_skip_engaged);
}

/* Settings and the FF chip share the same profile names. */
const char *ff_chip_text(void)
{
   static char buf[24];
#ifndef GPSP_PLAYABLE
   /* Harness builds only; a release build cannot reach bench mode. */
   if (g_pcfg.bench_mode)
      return "\xAF BENCH";
#endif
   snprintf(buf, sizeof(buf), "\xAF %s", pcfg_ff_name());
   return buf;
}

/* Hand frameskip back to the session policy — used when fast-forward (which
 * owns gpsp_frameskip while it runs, plan §4.4) releases it. */
void skip_policy_reapply(void)
{
   if (!g_net_up || !g_skip_engaged)
   {
      /* Outside a session, honour the sparse smoothing skip if the user asked
       * for one: draw N frames, skip 1.  A game a few fps short of 60 gets its
       * headroom back without the visible hitch that halving the rate causes.
       * A wireless session overrides it — the session's own skip policy is
       * about staying in step with the other console, which wins. */
      if (g_pcfg.frameskip_sparse > 0)
      {
         char n[12];   /* an int is up to 11 characters plus the terminator */
         snprintf(n, sizeof(n), "%d", g_pcfg.frameskip_sparse);
         fe_host_option_set_live("gpsp_frameskip_interval", n);
         skip_policy_set("sparse_interval");
      }
      else
         skip_policy_set("disabled");
      return;
   }
   if (g_pcfg.net_frameskip == SKIP_POL_AUTO)
      skip_policy_set("auto");
   else
   {
      fe_host_option_set_live("gpsp_frameskip_interval", "1");
      skip_policy_set("fixed_interval");
   }
}

void skip_policy_session_end(void)
{
   if (g_skip_engaged)
   {
      skip_policy_set("disabled");
      g_skip_engaged = 0;
   }
}

/* Once per main-loop iteration while a session is up.  Never runs during
 * fast-forward: FF owns gpsp_frameskip itself (plan §4.4). */
/* The rate this console is CURRENTLY TRYING to hit, x100 (ADR-0037).  Before
 * pace_init runs, and whenever the pacing module is off, that is the GBA's
 * nominal rate — which is also what `g_pace_target_x100` is initialised to,
 * so the zero-check is belt-and-braces against call order, not a real case. */
static unsigned skip_pace_target_x100(void)
{
   return g_pace_target_x100 ? g_pace_target_x100 : PACE_NOMINAL_X100;
}

void skip_policy_frame(void)
{
   uint64_t now, dt;
   unsigned frames, fps_x100, target, slow_x100, fast_x100;

   if (g_pcfg.net_frameskip != SKIP_POL_ADAPTIVE)
      return;
   if (g_ff_uncapped || g_ff_mult)
      return;
   if (ui_active())
   {
      /* The core is paused while the menu is open, so wall clock advances
       * and fe_host_frame_count() does not.  Left alone that divides real
       * seconds by zero frames and engages the skip on a fabricated
       * `fps=0.00` — which the field log duly showed.  Re-base the window
       * every menu frame so the first measurement after Resume is honest. */
      g_skip_win_us = net_now_us();
      g_skip_win_frames = fe_host_frame_count();
      return;
   }

   now = net_now_us();
   dt  = now - g_skip_win_us;
   if (dt < SKIP_WIN_US)
      return;

   frames = fe_host_frame_count() - g_skip_win_frames;
   fps_x100 = (unsigned)((uint64_t)frames * 100000000ull / dt);
   g_skip_win_us = now;
   g_skip_win_frames = fe_host_frame_count();

   /* ADR-0037: measure against what we are AIMING at, not against 59.7275. */
   target    = skip_pace_target_x100();
   slow_x100 = (unsigned)((uint64_t)target * SKIP_SLOW_PCT_TGT / 1000ull);
   fast_x100 = (unsigned)((uint64_t)target * SKIP_FAST_PCT_TGT / 1000ull);

   if (fps_x100 < slow_x100)
   {
      g_skip_fast = 0;
      if (!g_skip_engaged && ++g_skip_slow >= SKIP_SLOW_WINDOWS)
      {
         g_skip_engaged = 1;
         g_skip_slow = 0;
         fe_host_option_set_live("gpsp_frameskip_interval", "1");
         skip_policy_set("fixed_interval");
         /* `target=` is the whole point of ADR-0037: a field log must show
          * what the decision was measured AGAINST, or the next reader is
          * back to guessing which rate 29.43 was supposed to beat. */
         fe_evt("skip_engage fps=%u.%02u target=%u.%02u mode=fixed_interval "
                "interval=1", fps_x100 / 100, fps_x100 % 100,
                target / 100, target % 100);
      }
   }
   else if (fps_x100 >= fast_x100)
   {
      g_skip_slow = 0;
      if (g_skip_engaged && ++g_skip_fast >= SKIP_FAST_WINDOWS)
      {
         g_skip_engaged = 0;
         g_skip_fast = 0;
         skip_policy_set("disabled");
         fe_evt("skip_release fps=%u.%02u target=%u.%02u",
                fps_x100 / 100, fps_x100 % 100, target / 100, target % 100);
      }
   }
   else
      g_skip_slow = g_skip_fast = 0;   /* in the dead band: hold */
}

/* ---- session pacing (ADR-0033 fixed-rate; ADR-0027/0028 adaptive) --------
 *
 * TWO POLICIES LIVE HERE, selected by `config.ini net_pace_match`:
 *
 *   0  off       — both consoles free-run.  (Meaning unchanged.)
 *   1  FIXED     — DEFAULT (ADR-0033).  While a session is live both consoles
 *                  clamp to the SAME constant, `config.ini net_session_fps`
 *                  (default 40.00).  No negotiation, no control loop.
 *   2  adaptive  — ADR-0027/0028's peer-capability matcher, below, kept
 *                  verbatim for a hardware A/B.
 *
 * **THE MEANING OF `1` CHANGED.** It used to select the adaptive matcher.  An
 * existing config.ini carrying `net_pace_match=1` silently adopts fixed-rate
 * pacing, which is intended — 1 still means "pace during a session"; only the
 * how changed.  Anyone wanting the old behaviour must now write `2`.
 *
 * WHY THE CONTROL LOOP WENT AWAY.  The requirement changed: full-speed
 * emulation *during a wireless session* is now explicitly a nice-to-have, not
 * a requirement.  Trading is a temporary activity, and half speed for its
 * duration is acceptable so long as the session is not choppy, still accepts
 * input, and the emulator runs normally the moment the session ends.
 *
 * Against that requirement the matcher is not just unnecessary, it is
 * actively harmful.  Its target has to track a peer capability that genuinely
 * fluctuates with game workload (the field measured `peer_cap` walking
 * 49.9, 44.7, 41.6, 38.9, 41.1, 47.0, 51.0, 53.7, 55.4, 56.6, 52.6 ... within
 * seconds), so it spends the whole session hunting — and the applied rate
 * moving is itself a desync source, because Gen-3's RFU counts link timeouts
 * in FRAMES.  The hardware profile says why the hunt can never converge:
 * in-session steady state on a joining PSP-3000 is
 * `cpu 7754 vid 3069 blt 2973 audio 436` = 14253 us of a 16750 us budget, and
 * the host/join difference is entirely `cpu` (6071 -> 7754) — the GAME'S OWN
 * RFU driver code running on the emulated CPU.  That is not ours to optimise
 * and not ours to predict, so we stop predicting it.
 *
 * FIXED-RATE, then.  40.00 fps sits comfortably below the 45-58 fps both
 * consoles sustain in the join seat, and both sides compute it from the same
 * constant rather than from each other.  Deterministic, identical, and the
 * applied rate holds still for the whole session, which is the property the
 * link actually needs.  It costs speed on healthy sessions; per the changed
 * requirement, that is the trade we are choosing.
 *
 * RAMP, BOTH WAYS.  A step from 59.73 to 40.00 at session start is a stall by
 * another name — the frame it lands on is 8 ms longer, and the audio step
 * jumps with it.  So the applied rate GLIDES at PACE_FIXED_RAMP_X100_PER_S
 * (4.00 fps/s), ~5 s in and ~5 s out, on a per-frame time base rather than
 * the adaptive path's 1 s windows.  The glide out runs AFTER teardown, which
 * is why the throttle below gates on the applied target and never on
 * `g_net_up`: dropping the throttle instantly while the audio step is still
 * ramping would let the emulator outrun the consumer and overflow the ring.
 *
 * MISSING THE RATE IS REPORTED, NOT CHASED.  If a console cannot even sustain
 * the fixed rate we log `EVT session_pace_miss actual=` and carry on at the
 * configured clamp.  Chasing downward would reintroduce exactly the moving
 * target this ADR removed, and a known steady rate beats a correct-but-moving
 * one.  (The adaptive path's floor/low-water machinery is untouched and still
 * applies to mode 2.)
 *
 * AUDIO is unchanged in kind from ADR-0027: `g_audio_step` follows the
 * applied rate exactly, so production and consumption match and the stream
 * stays continuous, at the cost of a proportional pitch drop (~7 semitones at
 * 40.00 fps).  It glides because the pace glides.  The user approved this.
 *
 * ------------------------------------------------------------------------
 * What follows is mode 2's reasoning, unchanged (ADR-0027/0028):
 *
 * The field fact this exists for: the join/client role costs ~12 fps against
 * the host role on BOTH consoles (PSP-3000 58 host / 46 join; PSP-1000 56-58
 * host / 49-52 join), and the cost is inside the core's RFU client path, not
 * our transport (measured per-frame session cost totals ~500 us).  Two real
 * GBAs both run at 59.7275 Hz, so their link timing is mutually consistent.
 * Gen-3's RFU counts link timeouts in FRAMES, not wall-clock seconds — so
 * two emulators running ~20 % apart are permanently inconsistent in a way
 * two cartridges never are, and the slower side's game times out.  EQUALITY
 * MATTERS MORE THAN ABSOLUTE SPEED: if both run at the same rate, even a
 * slower one, the game-side link timing becomes mutually consistent again.
 *
 * THE CONTROL LOOP, and the trap in it.  Each side advertises a frame rate
 * and the faster side paces down to the slower.  If what a console
 * advertises is its *achieved* rate, the pair ratchets downward without
 * bound: A throttles to 46 to match B, A now measures and reports 46, B sees
 * "peer is at 46" and throttles itself, A re-measures lower, and both crawl
 * into the floor — a control-loop bug that looks exactly like a performance
 * regression.  So a console advertises its **capability**, never its
 * throttled rate:
 *
 *   capability = the rate this vblank-locked loop WOULD free-run at, derived
 *   from per-frame WORK time (the loop iteration with every vblank wait
 *   excluded — the same number ADR-0021 reports as `frame=`).  A frame
 *   costing w us occupies ceil(w / 16.683 ms) vblank periods, minimum one;
 *   sum that over a window and the rate is frames * 59.94 / periods.
 *
 * Our deliberate idle is not work, so throttling ourselves cannot move our
 * own advertised number.  `peer_cap` therefore always means "how fast my
 * partner CAN go", never "how fast my partner is currently choosing to go",
 * and the loop has exactly one mover: the genuinely faster console.  If
 * `self_cap` is ever seen sagging in the field log while `engaged=1`, the
 * ratchet is back and that line says so directly.
 *
 * WHAT THE FIELD CHANGED (ADR-0028).  The first hardware run proved the
 * mechanism and broke the control law.  Both consoles held `self_cap` flat
 * while throttling — the anti-ratchet is correct — and for the first time
 * both players entered the Union Room and could move.  But `peer_cap` swung
 * ~20 fps within seconds (49.9, 44.7, 41.6, 38.9, 41.1, 47.0, 51.0, 53.7,
 * 55.4, 56.6, 52.6, ...), and against a symmetric 2 fps/s ramp we were
 * permanently mid-chase, never at the right target, and even disengaged at
 * the floor mid-session.  The second log explained why: the console in the
 * JOIN seat becomes the slow one, so **the identity of the slow peer flips**
 * and a loop that tracks "whoever is behind right now" keeps reversing.
 *
 * So the target is no longer "the peer": it is the PAIR'S SUSTAINED WORST.
 *   - Each side keeps a decaying LOW-WATER MARK of both capabilities: a new
 *     low is taken immediately, and a low is only forgotten after 4 s of
 *     nothing worse, then at 0.50 fps per window.  This turns a 20 fps
 *     oscillation into a stable number.
 *   - The goal is `min(self_lo, peer_lo) + margin`.  **min() is symmetric**,
 *     so both consoles compute the SAME target from the same two numbers and
 *     a role flip does not move it — the identity of the mover changes, the
 *     target does not.  A steady 48 beats an accurate-but-moving 39-58.
 *   - ONE MOVER falls out for free instead of being enforced: whichever
 *     console is currently the binding constraint is already below the
 *     target and inserts no waits at all, because the throttle can only ever
 *     add delay.  No flag decides who moves.
 *
 * Guarantees, in the order they are enforced below:
 *   - ASYMMETRIC RAMP.  Fall 8.00 fps per window, rise 0.50.  Getting slow
 *     late is what desyncs a link; getting fast late costs nothing.
 *   - FLOOR.  We never pace ourselves below 40.00 fps — but we CLAMP there
 *     rather than releasing.  The field showed releasing is actively worse:
 *     against a peer at 38.9 it snaps us back to 59.73 and makes the gap
 *     21 fps instead of 1.1.  Absurd or zero reports are ignored outright.
 *   - CEILING.  The target never exceeds the GBA's own 59.7275 Hz.
 *   - STAY ENGAGED.  `engaged` is a wide-hysteresis *report* of "the target
 *     is meaningfully below nominal", not a gate on the throttle — the
 *     throttle follows the applied target alone.  Repeated engage/release
 *     cycling was itself a symptom; the 3000 released and never re-engaged
 *     even as it became the slower side.
 *   - RELEASE.  When the pair is genuinely fast again, or the peer goes
 *     away, the goal returns to nominal and the target ramps back off at the
 *     slow rate.  Nothing latches for the rest of the session.
 *
 * HOW THE TARGET IS APPLIED.  The loop is vblank-locked, so the only lever
 * is how many vblanks a frame waits for, and a whole extra vblank is a jump
 * from 59.94 to 29.97 fps — far too coarse.  Instead the fraction is
 * accumulated: a target needs 59.94/target vblanks per frame, so we carry
 * the fractional part in 1/10000ths and spend one extra vblank whenever it
 * crosses 1.  Target 46.50 -> 1.2890 vblanks/frame -> an extra vblank on
 * 28.9 % of frames, averaging exactly 46.50 fps.
 *
 * `config.ini net_pace_match` = 2 selects this policy (it was 1 before
 * ADR-0033), so the two policies A/B on hardware with no rebuild, exactly
 * like net_tx_thread/log_thread/sram_thread. */

/* The mode constants live in ff_psp.h: main() parses `net_pace_mode` from
 * config and so needs them.  The reasoning for each stays here. */
/* ADR-0078: FIXED's clamp, but the clamped rate FLOATS on backpressure.
 *
 * The fixed clamp (ADR-0033) bought rate equality with a constant, and the
 * constant has to be chosen for the worst case — 57.00 ships because the
 * join is ~2 fps short of nominal, so every pair pays 2.73 fps whether or
 * not their consoles could do better today.  The ADR-0027 matcher adapted,
 * but its signal was an EMA of the peer's ADVERTISED capability, and it
 * hunted (that is why ADR-0033 replaced it).
 *
 * This mode reuses ALL of FIXED's machinery — the glide, the audio step,
 * the snap, the miss reporting — and moves only WHERE the clamp value comes
 * from: the live reliable-TX backlog toward the peer (fe_np_txq_now).  A
 * receiver that stops absorbing shows up here within one RTO, as a direct
 * measurement rather than an estimate.  HOST-ONLY by design: the host is
 * the sender whose rate matters (child-side queues are child-only, HANDOVER
 * §2), and one adapting console cannot hunt against another — the join
 * simply runs its own fixed ceiling and the pair converges on what the join
 * actually absorbs.  On the join this mode degrades to plain FIXED.
 *
 * Control law, evaluated once per ~1 s pace window, asymmetric on purpose:
 *   backlog >= BP_HI            -> base falls  BP_STEP_DOWN, immediately
 *   backlog <= BP_LO for
 *     BP_RISE_WINS windows      -> base rises  BP_STEP_UP
 *   else                        -> hold, streak resets
 * Rise is slow (0.25 fps after 3 clean seconds), fall is fast (0.50 fps at
 * once), and every move is logged.  FALSIFIER (state it before the run):
 * if pace_bp shows the base oscillating — alternating up/down without a
 * change in conditions — this is ADR-0033's hunting again and the mode is
 * dead; if the base parks at the ini rate and never rises with txq at 0,
 * the signal is not informative and the mode is pointless. */
/* PACE_MODE_BACKPRESSURE: see ff_psp.h. */
#define PACE_BP_HI                6    /* payloads: ~0.1 s of frames backed up */
#define PACE_BP_LO                1
#define PACE_BP_STEP_DOWN_X100    50
#define PACE_BP_STEP_UP_X100      25
#define PACE_BP_RISE_WINS         3
#define PACE_BP_FLOOR_X100        5500
static unsigned g_bp_low_streak;
static unsigned g_bp_hold_wins;       /* windows since the last pace_bp line */

#define PACE_VBLANK_HZ_X100    5994   /* PSP display 59.94 Hz */
#define PACE_VBLANK_US        16683   /* one vblank period */
/* PACE_FLOOR_X100: see ff_psp.h -- main() clamps the ini rate against it. */
#define PACE_MARGIN_X100         50   /* aim just above the worst, not at it */
#define PACE_ENGAGE_X100        150   /* "engaged" once 1.50 below nominal... */
#define PACE_RELEASE_X100        40   /* ...and only released within 0.40 */
#define PACE_RAMP_DOWN_X100     800   /* fall 8.00 fps per window: react fast */
#define PACE_RAMP_UP_X100        50   /* rise 0.50 fps per window: recover slow */
#define PACE_LOW_HOLD_US    4000000   /* remember a peer's worst for 4 s... */
#define PACE_LOW_RISE_X100       50   /* ...then let it forget 0.50 fps/window */
#define PACE_WIN_US         1000000   /* ~1 s measurement window */
#define PACE_EMA_DEN              3   /* new sample gets 1/3 weight */
#define PACE_PEER_MIN_X100     1000   /* sanity wall on a reported value: */
#define PACE_PEER_MAX_X100    20000   /*   10.00 .. 200.00 fps, else ignore */
#define PACE_LOG_US        10000000   /* heartbeat the steady state this often */
#define PACE_MAX_EXTRA_VB         2   /* bound on extra vblanks per frame */

/* ADR-0033 fixed-rate mode. */
/* PACE_FIXED_RAMP_X100_PER_S: see ff_psp.h. */
#define PACE_MISS_MARGIN_X100      150 /* 1.50 fps of slack before "missed" */
#define PACE_MISS_LOG_US       10000000/* at most one miss line per 10 s */
/* ADR-0035: the snap only ever produces PACE_VBLANK_HZ_X100 / N.  N is capped
 * at 3 because N vblanks is (N-1) EXTRA vblanks and PACE_MAX_EXTRA_VB is 2 —
 * a snap the throttle could not actually deliver would be the same lie in a
 * different place. */
#define PACE_SNAP_MAX_VB            3

/* (state declared above sess_cost_evt, which reports it) */

/* Harness knob (.gpsp-harness.ini `pace_slow_us`).  The PPSSPP rig runs both
 * instances at full speed, so it cannot reproduce the field's host/join
 * asymmetry and pace matching would never engage there — nothing to observe.
 * Burning BUSY microseconds (not sleeping) on one instance manufactures a
 * genuinely slower peer whose cost lands in exactly the per-frame work time
 * the capability estimate reads, so the full loop is exercised. */
void pace_burn(void)
{
   uint64_t t0;
   if (!g_pace_slow_us)
      return;
   t0 = net_now_us();
   while (net_now_us() - t0 < (uint64_t)g_pace_slow_us)
      ;
}

void pace_audio_step(unsigned target_x100)
{
   /* One wall second at `target` fps contains target/59.7275 seconds of
    * emulated time, so the core produces that fraction of in_rate samples.
    * Consume at the same fraction and the ring neither drains nor floods. */
   if (target_x100 > PACE_NOMINAL_X100)
      target_x100 = PACE_NOMINAL_X100;
   g_audio_step = (unsigned)(((uint64_t)in_rate << 16) * target_x100
                             / ((uint64_t)OUT_RATE * PACE_NOMINAL_X100));
}

/* Decaying low-water mark (ADR-0028).  A new low is taken IMMEDIATELY — the
 * worst is what desyncs a link, so it must never be smoothed away.  A low is
 * only forgotten after PACE_LOW_HOLD_US of nothing worse, and then only at
 * PACE_LOW_RISE_X100 per window, which is what converts the field's 20 fps
 * swing into a target that holds still. */
static void pace_low_update(unsigned *lo, uint64_t *lo_us, unsigned v,
                            uint64_t now)
{
   if (!v)
      return;
   if (!*lo || v <= *lo)
   {
      *lo    = v;
      *lo_us = now;               /* restart the hold on every new low */
      return;
   }
   if (now - *lo_us >= PACE_LOW_HOLD_US)
   {
      /* Deliberately does NOT restart the hold: once the quiet period has
       * elapsed the mark keeps creeping up one step per window until it
       * meets the current value. */
      *lo += PACE_LOW_RISE_X100;
      if (*lo > v)
         *lo = v;
   }
}

/* Everything except the applied target and its audio step: those two are the
 * only state a glide is allowed to carry across a session boundary, and in
 * fixed-rate mode they must, or leaving a session would be a step change. */
static void pace_reset_state(void)
{
   g_pace_engaged     = 0;
   g_pace_cap_x100    = 0;
   g_pace_peer_x100   = 0;
   g_pace_self_lo     = 0;
   g_pace_peer_lo     = 0;
   g_pace_self_lo_us  = 0;
   g_pace_peer_lo_us  = 0;
   g_pace_acc         = 0;
   g_pace_win_vb      = 0;
   g_pace_floor_said  = 0;
   g_pace_miss_said   = 0;
   g_pace_win_us      = net_now_us();
   g_pace_win_frames  = fe_host_frame_count();
   g_pace_log_us      = g_pace_win_us;
   g_pace_ramp_us     = g_pace_win_us;
   fe_np_set_local_fps(0);            /* "unknown" until the first window */
}

/* Snap everything back to nominal.  Used by mode 0/2, where a session
 * boundary has always been a step (mode 2's ramp only ever moved the target
 * *within* a session). */
static void pace_reset(void)
{
   pace_reset_state();
   g_pace_target_x100 = PACE_NOMINAL_X100;
   g_pace_goal_x100   = PACE_NOMINAL_X100;
   pace_audio_step(PACE_NOMINAL_X100);
}

/* ADR-0035 — SNAP THE REQUESTED RATE TO ONE THE THROTTLE CAN ACTUALLY HOLD.
 *
 * The field ran a 40.00 target and achieved 35-38 fps with `self_cap=52-57`,
 * i.e. the console could have gone faster and still missed.  That is not a
 * control bug, it is QUANTIZATION.  We pace by waiting whole vblanks, so a
 * frame costs 1 or 2 of them; averaging 40 needs ~half the frames to finish
 * inside one 16.68 ms vblank, and when per-frame work sits just ABOVE that,
 * none do — every frame becomes 2 vblanks and the average collapses toward
 * 29.97 with jitter from the frames that occasionally take 3.
 *
 * The rates a console can hold with NO headroom assumption are exactly
 * `PACE_VBLANK_HZ / N`.  That set is sparse — 59.94, 29.97, 19.98 — and there
 * is deliberately nothing between nominal and 29.97: with whole-vblank pacing
 * there CANNOT be.  Anything else is only reachable when frames genuinely fit
 * in one vblank, which is exactly the assumption the field just falsified.
 *
 * NOTE FOR ANYONE READING THE FIELD CONFIG: 2 vblanks is **29.97**, not 29.86.
 * 29.86 is 59.7275/2 — the GBA's own frame rate halved — but the thing we
 * insert is a PSP DISPLAY vblank at 59.94 Hz, so the achievable rate is
 * 59.94/2.  A configured 29.86 snaps to 29.97 and the snap is logged; the
 * 0.11 fps is not worth a second mechanism, but the log must not be silent
 * about it or the next reader will think the clamp drifted.
 *
 * Returns the applied rate.  `*vb` receives the vblanks-per-frame it implies
 * (1 = nominal, i.e. not pacing at all). */
unsigned pace_snap_x100(unsigned req_x100, unsigned *vb)
{
   unsigned n, best_n = 1, best_err = 0, first = 1;

   if (vb)
      *vb = 1;
   if (!g_pcfg.net_session_fps_snap)
      return req_x100;                 /* A/B: apply the request verbatim */

   /* A request within the engage band of nominal is not a request to pace,
    * and snapping it DOWN to 29.97 would be a wild overreaction. */
   if (req_x100 + PACE_ENGAGE_X100 > PACE_NOMINAL_X100)
      return PACE_NOMINAL_X100;

   /* Otherwise the user does want pacing, so never snap back UP to nominal:
    * search N >= 2 only.  Nearest by ERROR IN THE RATE, not by rounding the
    * divisor — rounding 59.94/40.00 = 1.4985 to N=1 would hand back nominal
    * and silently cancel the clamp. */
   for (n = 2; n <= PACE_SNAP_MAX_VB; n++)
   {
      unsigned rate = PACE_VBLANK_HZ_X100 / n;
      unsigned err  = (rate > req_x100) ? rate - req_x100 : req_x100 - rate;
      if (first || err < best_err)
      {
         first    = 0;
         best_err = err;
         best_n   = n;
      }
   }
   if (vb)
      *vb = best_n;
   return PACE_VBLANK_HZ_X100 / best_n;
}

/* `EVT session_pace fps=<applied goal> reason=<session_start|session_end>` —
 * ADR-0033's one line for "the clamp changed".  It reports the rate being
 * ramped TO, which is the number that matters; `EVT pace_match ... why=ramp`
 * is not emitted in fixed mode because there is nothing to negotiate. */
static void pace_session_evt(const char *reason)
{
   FE_EVT_ONLY(reason);
   fe_evt("session_pace fps=%u.%02u reason=%s",
          g_pace_goal_x100 / 100, g_pace_goal_x100 % 100, reason);
}

void pace_session_begin(void)
{
   /* ADR-0078: BACKPRESSURE is FIXED plus a floating clamp; it shares every
    * piece of session lifecycle with FIXED, including this one.  The ini
    * rate is the STARTING base; the control law moves it from there. */
   if (g_pcfg.net_pace_match != PACE_MODE_FIXED &&
       g_pcfg.net_pace_match != PACE_MODE_BACKPRESSURE)
   {
      pace_reset();
      return;
   }
   g_bp_low_streak = 0;
   g_bp_hold_wins  = 0;
   pace_reset_state();
   {
      unsigned req = (unsigned)g_pcfg.net_session_fps_x100, vb = 1;
      g_pace_fixed_x100 = pace_snap_x100(req, &vb);
      if (g_pace_fixed_x100 > PACE_NOMINAL_X100)
         g_pace_fixed_x100 = PACE_NOMINAL_X100;
      /* Log the snap whenever it moved the number.  A user who configured
       * 40.00 must be TOLD they are getting 29.97 — silently under-running a
       * requested rate is what produced the field's 35-38 fps mystery. */
      if (g_pace_fixed_x100 != req)
         fe_evt("session_pace_snap req=%u.%02u applied=%u.%02u vblanks=%u "
                "(achievable rates are %u.%02u/N)",
                req / 100, req % 100,
                g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100, vb,
                PACE_VBLANK_HZ_X100 / 100, PACE_VBLANK_HZ_X100 % 100);
   }
   g_pace_goal_x100 = g_pace_fixed_x100;
   /* The target is NOT snapped: it glides down from wherever it is (nominal,
    * or mid-glide if a previous session ended seconds ago). */
   pace_session_evt("session_start");
}

void pace_session_end(void)
{
   if (g_pcfg.net_pace_match != PACE_MODE_FIXED &&
       g_pcfg.net_pace_match != PACE_MODE_BACKPRESSURE)
   {
      pace_reset();
      return;
   }
   pace_reset_state();
   g_pace_goal_x100 = PACE_NOMINAL_X100;
   pace_session_evt("session_end");
   /* The glide back up runs from the main loop after teardown — see
    * pace_fixed_ramp(), which is deliberately NOT gated on g_net_up. */
}

static void pace_log(const char *why)
{
   FE_EVT_ONLY(why);
   /* `*_lo` are the sustained worsts the target is actually derived from —
    * log them beside the instantaneous values so the field can see directly
    * whether the low-water marks are doing their job (ADR-0028). */
   fe_evt("pace_match target=%u.%02u self_cap=%u.%02u peer_cap=%u.%02u "
          "self_lo=%u.%02u peer_lo=%u.%02u engaged=%d why=%s",
          g_pace_target_x100 / 100, g_pace_target_x100 % 100,
          g_pace_cap_x100 / 100,    g_pace_cap_x100 % 100,
          g_pace_peer_x100 / 100,   g_pace_peer_x100 % 100,
          g_pace_self_lo / 100,     g_pace_self_lo % 100,
          g_pace_peer_lo / 100,     g_pace_peer_lo % 100,
          g_pace_engaged, why);
   g_pace_log_us = net_now_us();
}

/* Accumulates this frame's work cost and closes a ~1 s measurement window.
 * Returns 1 exactly when a window just closed, having refreshed
 * `g_pace_cap_x100` (CAPABILITY — see the header) and published it to the
 * peer; `*achieved_x100` is then the rate this console actually DELIVERED
 * over the window (wall clock, throttle waits included), which is the number
 * the fixed-rate miss check needs and the one capability deliberately is not.
 *
 * Both policies share this: mode 2 needs the capability, mode 1 needs the
 * achieved rate, and both want `self_cap` in `sess_cost` either way. */
int pace_window(uint32_t work_us, unsigned *achieved_x100)
{
   uint64_t now, dt;
   unsigned frames, sample, vb;

   /* Cost this frame in whole vblank periods — what the loop actually pays,
    * and what a free-running loop would pay.  Never less than one. */
   vb = (unsigned)((work_us + PACE_VBLANK_US - 1) / PACE_VBLANK_US);
   if (vb < 1)
      vb = 1;
   g_pace_win_vb += vb;

   if (ui_active())
   {
      /* Core paused, wall clock still moving: re-base or the next window
       * divides real seconds by zero frames (the same trap skip_policy_frame
       * fell into in the field). */
      g_pace_win_us     = net_now_us();
      g_pace_win_frames = fe_host_frame_count();
      g_pace_win_vb     = 0;
      return 0;
   }

   now = net_now_us();
   dt  = now - g_pace_win_us;
   if (dt < PACE_WIN_US)
      return 0;

   frames = fe_host_frame_count() - g_pace_win_frames;
   if (!frames || !g_pace_win_vb)
   {
      g_pace_win_us     = now;
      g_pace_win_frames = fe_host_frame_count();
      g_pace_win_vb     = 0;
      return 0;
   }

   /* CAPABILITY, not achieved fps: our own throttle waits are not work and
    * so cannot appear here.  This is what stops the pair ratcheting down. */
   sample = (unsigned)((uint64_t)frames * PACE_VBLANK_HZ_X100 / g_pace_win_vb);
   if (sample > PACE_NOMINAL_X100)
      sample = PACE_NOMINAL_X100;      /* we never run the game faster */
   g_pace_cap_x100 = g_pace_cap_x100
      ? (g_pace_cap_x100 * (PACE_EMA_DEN - 1) + sample) / PACE_EMA_DEN
      : sample;

   /* ACHIEVED: frames per wall second, x100.  dt >= PACE_WIN_US so no /0. */
   *achieved_x100 = (unsigned)((uint64_t)frames * 100000000ull / dt);

   g_pace_win_us     = now;
   g_pace_win_frames = fe_host_frame_count();
   g_pace_win_vb     = 0;

   /* Publish OUR capability. The peer paces against this. */
   fe_np_set_local_fps(g_pace_cap_x100);
   return 1;
}

/* ADR-0033: glide the applied rate toward the goal on a per-frame time base.
 * Runs whether or not a session is up — the glide OUT happens after teardown
 * by construction, and gating this on `g_net_up` would turn leaving a session
 * back into the step change the ramp exists to avoid. */
static void pace_fixed_ramp(void)
{
   uint64_t now = net_now_us(), dt;
   unsigned step, was = g_pace_target_x100;

   /* Pre-boot both of these are still zero, and this runs from the FIRST
    * frame — before any session, because the glide out has to.  A zero goal
    * reads as "ramp to 0 fps", so the loop would throttle a solo game to the
    * PACE_MAX_EXTRA_VB floor with no session in sight.  Nominal is the only
    * safe reading of "not set yet"; the session sets a real goal. */
   if (!g_pace_target_x100)
      g_pace_target_x100 = PACE_NOMINAL_X100;
   if (!g_pace_goal_x100)
      g_pace_goal_x100 = PACE_NOMINAL_X100;
   if (!g_pace_ramp_us)
   {
      g_pace_ramp_us = now;
      return;
   }
   if (g_pace_target_x100 == g_pace_goal_x100)
   {
      g_pace_ramp_us = now;                       /* parked: no debt banked */
      return;
   }

   dt   = now - g_pace_ramp_us;
   step = (unsigned)((uint64_t)PACE_FIXED_RAMP_X100_PER_S * dt / 1000000ull);
   if (!step)
      return;   /* sub-step: leave g_pace_ramp_us alone so dt keeps growing,
                 * or a fast enough loop would never accumulate a whole step */
   g_pace_ramp_us = now;

   if (g_pace_target_x100 > g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_target_x100 - g_pace_goal_x100 > step)
            ? g_pace_target_x100 - step : g_pace_goal_x100;
   else
      g_pace_target_x100 =
         (g_pace_goal_x100 - g_pace_target_x100 > step)
            ? g_pace_target_x100 + step : g_pace_goal_x100;

   if (g_pace_target_x100 != was)
   {
      /* Audio follows the APPLIED rate exactly (ADR-0027 §audio): production
       * and consumption then match and the stream stays continuous. */
      pace_audio_step(g_pace_target_x100);
      /* `engaged` is a report only, same as mode 2. */
      g_pace_engaged =
         (g_pace_target_x100 + PACE_ENGAGE_X100 <= PACE_NOMINAL_X100);
      if (g_pace_target_x100 == g_pace_goal_x100)
         pace_session_evt(g_pace_goal_x100 >= PACE_NOMINAL_X100
                          ? "ramp_done_nominal" : "ramp_done");
   }
}

/* ADR-0033 sanity: report a console that cannot hold the fixed rate, and do
 * NOT chase it downward.  Chasing is exactly the moving target this policy
 * removed, and the user prefers a known steady rate to a correct-but-moving
 * one.  Only checked once the glide has settled — during the ramp the
 * achieved rate is legitimately not the clamp yet. */
static void pace_fixed_miss(unsigned achieved_x100, uint64_t now)
{
   if (g_pace_target_x100 != g_pace_goal_x100 || !achieved_x100)
      return;
   if (achieved_x100 + PACE_MISS_MARGIN_X100 >= g_pace_goal_x100)
   {
      g_pace_miss_said = 0;            /* recovered: re-arm the report */
      return;
   }
   if (g_pace_miss_said && now - g_pace_log_us < PACE_MISS_LOG_US)
      return;
   g_pace_miss_said = 1;
   g_pace_log_us    = now;
   fe_evt("session_pace_miss actual=%u.%02u fixed=%u.%02u self_cap=%u.%02u"
          " — not chasing",
          achieved_x100 / 100,     achieved_x100 % 100,
          g_pace_goal_x100 / 100,  g_pace_goal_x100 % 100,
          g_pace_cap_x100 / 100,   g_pace_cap_x100 % 100);
}

/* ADR-0078: one window's worth of backpressure decision.  Adjusts the
 * FLOATING base (g_pace_fixed_x100) that the fixed-mode glide then follows;
 * never touches g_pace_target_x100 directly, so audio and throttle stay in
 * the lockstep pace_extra_vblanks() documents. */
static void pace_bp_window(void)
{
   uint32_t q = fe_np_txq_now();
   unsigned was = g_pace_fixed_x100;
   const char *why = NULL;

   FE_EVT_ONLY(was);

   if (!g_net_is_host)
      return;                     /* join: plain FIXED at the ini ceiling */

   if (q >= PACE_BP_HI)
   {
      g_bp_low_streak = 0;
      if (g_pace_fixed_x100 > PACE_BP_FLOOR_X100)
      {
         g_pace_fixed_x100 =
            (g_pace_fixed_x100 - PACE_BP_FLOOR_X100 > PACE_BP_STEP_DOWN_X100)
               ? g_pace_fixed_x100 - PACE_BP_STEP_DOWN_X100
               : PACE_BP_FLOOR_X100;
         why = "down";
      }
   }
   else if (q <= PACE_BP_LO)
   {
      if (++g_bp_low_streak >= PACE_BP_RISE_WINS)
      {
         g_bp_low_streak = 0;
         if (g_pace_fixed_x100 < PACE_NOMINAL_X100)
         {
            g_pace_fixed_x100 =
               (PACE_NOMINAL_X100 - g_pace_fixed_x100 > PACE_BP_STEP_UP_X100)
                  ? g_pace_fixed_x100 + PACE_BP_STEP_UP_X100
                  : PACE_NOMINAL_X100;
            why = "up";
         }
      }
   }
   else
      g_bp_low_streak = 0;

   if (why)
   {
      g_pace_goal_x100 = g_pace_fixed_x100;
      g_bp_hold_wins   = 0;
      fe_evt("pace_bp txq=%u base=%u.%02u was=%u.%02u why=%s",
             (unsigned)q, g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100,
             was / 100, was % 100, why);
   }
   else if (++g_bp_hold_wins >= 10)
   {
      /* ADR-0058's rule: a probe whose silence looks like a dead instrument
       * is not evidence.  One hold line per ~10 s says the law is running. */
      g_bp_hold_wins = 0;
      fe_evt("pace_bp txq=%u base=%u.%02u why=hold streak=%u",
             (unsigned)q, g_pace_fixed_x100 / 100, g_pace_fixed_x100 % 100,
             g_bp_low_streak);
   }
}

/* Called once per main-loop iteration with the work time of the frame just
 * finished (vblank waits excluded). */
void pace_frame(uint32_t work_us)
{
   uint64_t now;
   unsigned achieved = 0;
   unsigned peer, desired;
   int was_engaged;
   unsigned was_target;

   if (g_pcfg.net_pace_match == PACE_MODE_FIXED ||
       g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE)
   {
      pace_fixed_ramp();               /* every frame, session or not */
      if (g_net_up && pace_window(work_us, &achieved))
      {
         /* Nothing is negotiated in this mode, but keep reading the peer's
          * advertised capability: `sess_cost pace=` still reports it, and
          * "what could the pair actually have sustained" is the one question
          * a fixed clamp cannot answer for itself. */
         peer = fe_np_peer_min_fps();
         g_pace_peer_x100 =
            (peer < PACE_PEER_MIN_X100 || peer > PACE_PEER_MAX_X100) ? 0 : peer;
         if (g_pcfg.net_pace_match == PACE_MODE_BACKPRESSURE)
            pace_bp_window();          /* ADR-0078: move the base, maybe */
         pace_fixed_miss(achieved, net_now_us());
      }
      return;
   }

   if (!g_net_up)
      return;
   if (!pace_window(work_us, &achieved))
      return;

   was_engaged = g_pace_engaged;
   was_target  = g_pace_target_x100;
   now         = net_now_us();

   /* ---- decide the goal ------------------------------------------------ */
   peer = fe_np_peer_min_fps();
   if (peer < PACE_PEER_MIN_X100 || peer > PACE_PEER_MAX_X100)
      peer = 0;                        /* absurd, stale or silent: ignore */
   g_pace_peer_x100 = peer;

   /* Both sides' sustained worst.  Ours is tracked even with no peer, so a
    * target is available the instant one appears. */
   pace_low_update(&g_pace_self_lo, &g_pace_self_lo_us, g_pace_cap_x100, now);
   if (peer)
      pace_low_update(&g_pace_peer_lo, &g_pace_peer_lo_us, peer, now);

   if (!g_pcfg.net_pace_match || !peer || !g_pace_peer_lo)
   {
      g_pace_goal_x100 = PACE_NOMINAL_X100;
      g_pace_engaged   = 0;
   }
   else
   {
      /* THE PAIR'S sustained worst, not "the peer's".  min() is symmetric,
       * so both consoles land on the same number and a role flip does not
       * move it (ADR-0028). */
      unsigned pair = (g_pace_self_lo && g_pace_self_lo < g_pace_peer_lo)
                      ? g_pace_self_lo : g_pace_peer_lo;

      desired = pair + PACE_MARGIN_X100;
      if (desired < PACE_FLOOR_X100)
      {
         /* CLAMP, never release: snapping back to nominal against a peer
          * this slow widens the gap instead of closing it. */
         desired = PACE_FLOOR_X100;
         if (!g_pace_floor_said)
         {
            g_pace_floor_said = 1;
            fe_evt("pace_floor pair=%u.%02u floor=%u.%02u — clamped, still pacing",
                   pair / 100, pair % 100,
                   PACE_FLOOR_X100 / 100, PACE_FLOOR_X100 % 100);
         }
      }
      else
         g_pace_floor_said = 0;
      if (desired > PACE_NOMINAL_X100)
         desired = PACE_NOMINAL_X100;

      g_pace_goal_x100 = desired;

      /* `engaged` only REPORTS that the goal is meaningfully below nominal;
       * the throttle follows the applied target, so this cannot cause the
       * engage/release cycling the field showed. */
      if (!g_pace_engaged)
      {
         if (g_pace_goal_x100 + PACE_ENGAGE_X100 <= PACE_NOMINAL_X100)
            g_pace_engaged = 1;
      }
      else if (g_pace_goal_x100 + PACE_RELEASE_X100 > PACE_NOMINAL_X100)
         g_pace_engaged = 0;
   }

   /* ---- ramp the applied target: fall fast, rise slow ------------------- */
   if (g_pace_target_x100 > g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_target_x100 - g_pace_goal_x100 > PACE_RAMP_DOWN_X100)
            ? g_pace_target_x100 - PACE_RAMP_DOWN_X100 : g_pace_goal_x100;
   else if (g_pace_target_x100 < g_pace_goal_x100)
      g_pace_target_x100 =
         (g_pace_goal_x100 - g_pace_target_x100 > PACE_RAMP_UP_X100)
            ? g_pace_target_x100 + PACE_RAMP_UP_X100 : g_pace_goal_x100;

   if (g_pace_target_x100 != was_target)
      pace_audio_step(g_pace_target_x100);

   if (g_pace_target_x100 != was_target)
      pace_log(g_pace_engaged != was_engaged
               ? (g_pace_engaged ? "engage" : "release") : "ramp");
   else if (g_pace_engaged != was_engaged)
      pace_log(g_pace_engaged ? "engage" : "release");
   else if (g_pace_engaged && now - g_pace_log_us >= PACE_LOG_US)
      pace_log("hold");
}

/* How many EXTRA vblanks this frame should wait for, beyond the one the loop
 * always takes.  Fractional accumulator: see the header comment. */
unsigned pace_extra_vblanks(void)
{
   unsigned v_x10000, frac, n = 0;

   if (!g_pcfg.net_pace_match)
      return 0;
   /* Mode 2's target is snapped back to nominal at teardown, so this is
    * belt-and-braces there.  In FIXED mode there is deliberately no g_net_up
    * gate: the glide out of the clamp runs after teardown, and cutting the
    * throttle the instant the session ends — while the audio step is still
    * several seconds from nominal — is precisely the ring-overflow the
    * lockstep note below is about. */
   if (g_pcfg.net_pace_match == PACE_MODE_ADAPTIVE && !g_net_up)
      return 0;
   /* Gate on the APPLIED TARGET, not on `engaged`.  `engaged` moves the goal;
    * the target is what both this throttle and the audio step follow, and
    * they must stay in lockstep.  Releasing by clearing `engaged` alone would
    * drop the throttle instantly while the audio step was still ramping back
    * over several seconds — the emulator would then outrun the consumer and
    * the ring would overflow (dropped tail = clicks) for the whole ramp. */
   if (g_pace_target_x100 >= PACE_NOMINAL_X100 || g_pace_target_x100 == 0)
      return 0;                        /* not throttling: leave pacing alone */

   v_x10000 = (unsigned)((uint64_t)PACE_VBLANK_HZ_X100 * 10000u
                         / g_pace_target_x100);
   frac = (v_x10000 > 10000u) ? v_x10000 - 10000u : 0u;
   g_pace_acc += frac;
   while (g_pace_acc >= 10000u && n < PACE_MAX_EXTRA_VB)
   {
      g_pace_acc -= 10000u;
      n++;
   }
   if (g_pace_acc > 20000u)
      g_pace_acc = 0;                  /* pathological target: never bank debt */
   return n;
}

/* ADR-0034: `EVT blit_prof` on the same 600-frame cadence as `core_prof`, so
 * the two line up in the field log and the blit's share of the frame can be
 * read directly against `core_phase`'s `blt` phase.
 *
 * `stage` is the CPU-side conversion (plus, in mode 0 only, the 82 KiB cache
 * writeback) — the part `blit_mode` moves.  `gu` is the list build and the
 * sceGuSync that blocks until the GE has finished; no placement change can
 * shorten that, so if `gu` dominates then the staging A/B is the wrong hunt
 * and the answer is in the GE, not the copy.  Reporting them separately is
 * the point: one number could not tell those two stories apart. */

/* ---- reporting surface --------------------------------------------------
 *
 * The telemetry in main_psp.c (EVT sess_cost, EVT frame_hist) and one harness
 * ini knob are the only things outside this file that need to see pacing state.
 * They get these five reads and one write instead of the variables, so nothing
 * outside can steer the engine by assignment. */
unsigned ff_pace_target_x100(void) { return g_pace_target_x100; }
unsigned ff_pace_cap_x100(void)    { return g_pace_cap_x100; }
unsigned ff_pace_peer_x100(void)   { return g_pace_peer_x100; }
int      ff_pace_engaged(void)     { return g_pace_engaged; }
int      ff_pace_slow_us(void)     { return g_pace_slow_us; }

void ff_pace_set_slow_us(int us)
{
   if (us < 0)     us = 0;
   if (us > 30000) us = 30000;   /* the clamp main() used to apply itself */
   g_pace_slow_us = us;
}
