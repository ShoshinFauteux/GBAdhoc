/* fe_gbadual.h -- the GBA link mirror's second machine in a frontend
 * (GBA_LINK builds only; docs/GBA-LINK-MIRROR.md).
 *
 * The local machine is the core as the frontend already runs it (instance A,
 * slot 0 here).  This module brings up instance B (the renamed copy of the
 * core) on the same cartridge, starts it from the same save state, and steps
 * the two through gbalink_pair with the cable between them.
 *
 * Milestone 3 (this file): the bench form, no network -- the partner's
 * buttons come from the player (mirror) or from a frame-stamped script.  The
 * network session will feed the same pair.
 */
#ifndef FE_GBADUAL_H
#define FE_GBADUAL_H

#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fe_gbadual_config
{
   const char *rom_path;        /* the cartridge (same as instance A's)     */
   const char *script;          /* partner's button script, or NULL         */
   /* ...or the partner's AUTOPILOT script (fe_autopilot grammar, RAM
    * predicates read the partner's machine); wins over `script`. */
   const char *autopilot;
   int         mirror;          /* partner gets the player's buttons        */
   /* 0 (default): B reads instance A's ROM pool (gbalink.h shared ROM).
    * 1: B loads its own copy of the cartridge (the A/B arm for sharing). */
   int         own_rom;
   uint32_t    lookahead;       /* cycles; 0 = default                       */
   const char *hash_path;       /* per-frame hash of both machines, or NULL */
   /* The frontend's environment callback: instance B asks it the same
    * questions (BIOS directory, core options), so both machines are set up
    * alike; requests that would re-point frontend state are not passed. */
   retro_environment_t env;
   void      (*log)(const char *fmt, ...);
   /* 1: a two-console session (fe_gblink with fe_gbadual_machines) starts
    * the pair; a state load no longer restarts the bench pair. */
   int         session;
   /* 1: the headless partner mixes its sound as before (kill switch for
    * sound.h sound_synth_off; harness/config gba_link_headless_audio = 0). */
   int         partner_audio;   /* -1: silent AND its ring is not read out
                                 * (gba_link_headless_audio = 2, section 23) */
   /* 1: the session's save watch hashes the whole battery image every 30
    * frames (kill switch for the write count; gba_link_sram_crc = 1). */
   int         sram_crc;
   /* Instance B's SMALL-tier translation cache offset mod 8 KiB, PLUS ONE
    * (0 = the build's JIT_PIN_OFFSET, as instance A).  LAYOUT_PIN link
    * builds only (cpu_threaded.c jit_small_pin_at); harness/config
    * gba_link_jit_pin_b.  GBA-LINK-MIRROR.md 17.5. */
   unsigned    b_jit_pin_p1;
   /* 1: the cable fast path in both cores (gbalink_core.c gbalink_fast;
    * harness/config gba_link_fast).  0 = the path every hardware run so far
    * used.  GBA-LINK-MIRROR.md 21. */
   int         cable_fast;      /* 0 off, 1 on, 2 the checking mode,
                                 * 3 on + the lean inline hooks (section 23) */
   /* Progress guard (section 22): the clock, and the longest one pair step
    * may take in milliseconds (0 = 15000; harness gba_link_step_max_ms,
    * negative = no wall-clock bound). */
   uint64_t  (*time_us)(void);
   int         step_max_ms;
} fe_gbadual_config;

/* After instance A has loaded its cartridge and save.  0 = B is up. */
int  fe_gbadual_load(const fe_gbadual_config *cfg);
/* Section 19: instance B's static state belongs to the session.
 * fe_gbadual_boot: once, first thing in the process (B's pristine .data).
 * fe_gbadual_lend(1): lend B's zero-initialised block to instance A's ROM
 * cache (call before A's retro_load_game; withdraw with 0 only after A's
 * ROM cache is gone).  fe_gbadual_load refuses while it is lent. */
void   fe_gbadual_boot(void);
void   fe_gbadual_lend(int on);
int    fe_gbadual_lent(void);
size_t fe_gbadual_lendable(void);
/* B loaded (fe_gbadual_load ran) / give back everything it took. */
int    fe_gbadual_loaded(void);
void   fe_gbadual_unload(void);
/* After instance A loaded a state from `state` (n bytes): B loads the same
 * state and save RAM, and the pair (re)starts with session time 0. */
int  fe_gbadual_state_loaded(const void *state, size_t n);
/* 1 while the pair is running (from the first state load). */
int  fe_gbadual_active(void);
int  fe_gbadual_running_inst(void);   /* DRPROF_HW: 0 A, 1 B, -1 none */
/* One display frame: the local machine's buttons (GBA KEYINPUT order, 1 =
 * pressed) for its next frame; steps until the local machine finished that
 * frame.  0 = ok, -1 = the pair stopped (error logged). */
int  fe_gbadual_frame(uint16_t local_keys);
/* Telemetry lines (heartbeat cadence): `gbadual ...` and `gbadual_health`. */
void fe_gbadual_stats_evt(void (*evt)(const char *fmt, ...));
/* One line for a watchdog thread: which slot is inside run(), runs this
 * step, frames, switches, the partner's guest pc (docs/GBA-LINK-MIRROR.md
 * section 13).  Safe to call from another thread. */
int  fe_gbadual_probe(char *buf, size_t n);
void fe_gbadual_close(void);
/* The pair stopped on an error (gbalink_pair E_*; 0 = none): a bench run
 * ends on it (the session layer ends its own, fe_gblink `gblink_failed`). */
int  fe_gbadual_fault(void);
/* From a watchdog thread: ask both cores to stop at their next scheduler
 * call (gbalink_core.c gbalink_abort).  The pair then fails E_OVERRUN. */
void fe_gbadual_abort(void);

/* The two-console session's machines (fe_gblink.h): the local machine is
 * instance A in slot `local_slot`, the partner instance B in the other.
 * Needs fe_gbadual_load with cfg.session = 1.  NULL when B is not loaded. */
struct fe_gblink_machines;
const struct fe_gblink_machines *fe_gbadual_machines(void);

/* retropad bitmask -> GBA KEYINPUT order */
uint16_t fe_gbadual_keys_from_retro(uint32_t pad);

/* Instance B's gamepak_page_loads (the objcopy gbab_ rename of
 * gba_memory.o, psp/Makefile): 32 KiB ROM page faults served.
 * fe_gbadual.c asserts its type equals instance A's. */
extern uint32_t gbab_gamepak_page_loads;

#ifdef __cplusplus
}
#endif
#endif
