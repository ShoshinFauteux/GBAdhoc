/* fe_gbadual.c -- see fe_gbadual.h.  GBA_LINK builds only. */
#ifdef GBA_LINK

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "fe_gbadual.h"
#include "fe_gblink.h"
#include "fe_console.h"
#include "../gbalink.h"
#include "../gbalink/gbalink_pair.h"
#include "../gbalink/gbalink_script.h"
#define GBAD_WEAK_EARLY __attribute__((weak))

/* Instance B: the core relocatable with every global renamed gbab_*
 * (psp/Makefile, tools/gbamirror/Makefile). */
void   gbab_retro_set_environment(retro_environment_t);
void   gbab_retro_set_video_refresh(retro_video_refresh_t);
void   gbab_retro_set_audio_sample(retro_audio_sample_t);
void   gbab_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void   gbab_retro_set_input_poll(retro_input_poll_t);
void   gbab_retro_set_input_state(retro_input_state_t);
void   gbab_retro_init(void);
bool   gbab_retro_load_game(const struct retro_game_info *);
void   gbab_retro_unload_game(void);
void   gbab_retro_deinit(void);
void   gbab_dynarec_release_translation_caches(void) __attribute__((weak));
void   gbab_gamepak_share_release(void) __attribute__((weak));
void   gbab_dynarec_heap_tables_release(void) __attribute__((weak));
bool   gbab_retro_unserialize(const void *, size_t);
size_t gbab_retro_serialize_size(void);
bool   gbab_retro_serialize(void *, size_t);
bool   retro_serialize(void *, size_t);
bool   retro_unserialize(const void *, size_t);
size_t retro_serialize_size(void);
void  *gbab_retro_get_memory_data(unsigned);
void  *retro_get_memory_data(unsigned);
extern uint32_t gbab_gamepak_buffer_cap;
/* shared ROM (gba_memory.c) */
extern const gbalink_rom_share *gbab_gamepak_share_in;
extern void (*gamepak_evict_hook)(uint32_t);
void gamepak_share_export(gbalink_rom_share *);
void gbab_gamepak_share_unmap(uint32_t);
extern uint32_t gamepak_page_loads, gbab_gamepak_page_loads;
#if defined(PSP)
extern uint32_t gbab_jit_cache_force_small;
/* Instance B's static state (psp/gbalink_core_b.ld, GBA-LINK-MIRROR.md 19).
 * Weak: only the PSP's scripted partial link defines them. */
extern char gbab_bdata_start[] __attribute__((weak));
extern char gbab_bdata_end[] __attribute__((weak));
extern char gbab_bdata_pristine[] __attribute__((weak));
extern char gbab_bregion_start[] __attribute__((weak));
extern char gbab_bregion_lend[] __attribute__((weak));
extern uint8_t *gamepak_spare_pool2;
extern uint32_t gamepak_spare_pool2_bytes;
extern int32_t gbab_jit_pin_at __attribute__((weak));
extern uint8_t *gbab_rom_translation_cache;
extern uint32_t gbab_rom_cache_watermark;
extern uint32_t gbalink_headless_audio, gbab_gbalink_headless_audio;
extern uint32_t gbalink_fast, gbab_gbalink_fast;
extern volatile uint32_t gbalink_abort, gbab_gbalink_abort;
/* SCHED_FAST builds: the partner's scheduler switch follows the player's */
extern uint32_t sched_fast GBAD_WEAK_EARLY, gbab_sched_fast GBAD_WEAK_EARLY;
extern uint32_t irq_io_fast GBAD_WEAK_EARLY, gbab_irq_io_fast GBAD_WEAK_EARLY;
extern uint32_t msr_fast GBAD_WEAK_EARLY, gbab_msr_fast GBAD_WEAK_EARLY;
extern uint32_t timer_lazy GBAD_WEAK_EARLY, gbab_timer_lazy GBAD_WEAK_EARLY;
extern uint32_t backup_writes, gbab_backup_writes;
/* cache_paranoid (harness): every sync whole D+I.  The frontend sets A's;
 * B must get the same, or an A/B arm tests only half the code. */
extern uint32_t platform_cache_paranoid, gbab_platform_cache_paranoid;
#endif

/* HANG STUDY (docs/GBA-LINK-MIRROR.md section 13).  Weak: present in the
 * PSP cores (BADJUMP_SAFE, the GBA_LINK cache counters), absent elsewhere. */
#define GBAD_WEAK __attribute__((weak))
/* BADJUMP_SAFE: a derailed guest is pointed at the reset vector and this flag
 * asks the FRONTEND for a real reset after the frame.  The frontend reads
 * only A's (fe_host_guest_fault_check); B's was never read, so a derailed
 * partner ran on as a "zombie" (the BIOS in the wrong mode with its hardware
 * live). */
extern volatile uint32_t badjump_reset_pending GBAD_WEAK;
extern volatile uint32_t gbab_badjump_reset_pending GBAD_WEAK;
extern uint32_t badjump_last_pc GBAD_WEAK, gbab_badjump_last_pc GBAD_WEAK;
extern uint32_t badjump_recoveries GBAD_WEAK, gbab_badjump_recoveries GBAD_WEAK;
extern uint32_t flush_rom_total GBAD_WEAK, gbab_flush_rom_total GBAD_WEAK;
extern uint32_t flush_ram_total GBAD_WEAK, gbab_flush_ram_total GBAD_WEAK;
extern uint32_t jit_big_i_calls GBAD_WEAK, gbab_jit_big_i_calls GBAD_WEAK;
extern uint32_t jit_big_i_max GBAD_WEAK, gbab_jit_big_i_max GBAD_WEAK;
extern uint32_t jit_big_d_calls GBAD_WEAK, gbab_jit_big_d_calls GBAD_WEAK;
extern uint32_t gbab_reg[] GBAD_WEAK;
#define GBAD_U(sym) (&(sym) ? (unsigned)(sym) : 0u)

/* The partner's autopilot: fe_autopilot.o renamed (psp/Makefile). */
int  gbab_fe_autopilot_load(const char *path);
void gbab_fe_autopilot_frame(void);
int  gbab_fe_autopilot_status(void);
static uint32_t b_ap_mask;
static gbalink_inst_ops *b_ops_for_ap;
void gbab_fe_host_input_inject(uint32_t m) { b_ap_mask = m; }
int gbab_fe_host_mem_read(uint32_t a, void *o, unsigned n)
{ return b_ops_for_ap ? b_ops_for_ap->peek(a, o, n) : -1; }
uint32_t gbab_fe_host_sram_crc_now(void)
{ return b_ops_for_ap ? b_ops_for_ap->sram_crc() : 0; }

#define SCRIPT_CAP 30000u    /* frames (~8 min); ~0.2 MB: the 1000 has ~1 MB after load */

static struct
{
   fe_gbadual_config cfg;
   int loaded, active, failed;
   int inited;                           /* gbab_retro_init has run      */
   gbalink_pair pair;
   gbalink_inst_ops ops[2];
   gbalink_script script;
   uint16_t local_ring[GBALINK_IN_RING];   /* mirror: player's keys by frame */
   uint32_t fed1;
   FILE *hash;
   int use_ap;
   uint32_t frames_local;
   gbalink_rom_share share;
   /* two-console session (fe_gbadual_machines) */
   int local_slot;                       /* which slot instance A is      */
   int sram_crc;                         /* m_cart_crc: 1 = hash the image */
   int (*sess_input)(void *, int, uint64_t, uint16_t *);
   void *sess_input_user;
   void (*sess_hook)(void *, int, uint64_t);
   void *sess_hook_user;
} D;

static void lg(const char *msg)
{
   if (D.cfg.log)
      D.cfg.log("%s", msg);
}

/* Stop the pair for good and give the frontend back a solo machine.  The
 * player's core must leave cable mode: while it is in it, every update_gba
 * postpones itself (gbalink_entry_yield) and the frontend's plain retro_run
 * would never complete a frame again. */
static int g_fault;       /* the pair's error when it stopped (survives D) */
static void sess_clock_restore(void);

int fe_gbadual_fault(void) { return g_fault; }

void fe_gbadual_abort(void)
{
   gbalink_abort = 1;
   gbab_gbalink_abort = 1;
}

/* Section 22: the pair's wall-clock bound. */
static void pair_guard_cfg(gbalink_pair_config *pc)
{
   int ms = D.cfg.step_max_ms;
   pc->now_us = D.cfg.time_us;
   pc->step_us_max = ms < 0 ? 0 : (uint64_t)(ms ? ms : 15000) * 1000u;
}

static void gbadual_stop(const char *why)
{
   char msg[200];
   g_fault = D.pair.error ? D.pair.error : -1;
   snprintf(msg, sizeof(msg), "gbadual_stop why=%s frames=%u/%u err=%d "
            "switches=%u", why, (unsigned)D.pair.frames[0],
            (unsigned)D.pair.frames[1], D.pair.error,
            (unsigned)D.pair.st.switches);
   lg(msg);
   D.failed = 1;
   if (D.active)
      gbalink_pair_end(&D.pair);
   D.active = 0;
   sess_clock_restore();
}

/* B asks the frontend what A asks (system directory, options, log), but
 * nothing that would re-point frontend state at B: memory maps, input
 * descriptors, the netpacket interface, messages, audio buffer status. */
static bool b_env(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_VARIABLE:
   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return D.cfg.env ? D.cfg.env(cmd, data) : false;
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
   default:
      return false;
   }
}

static void b_video(const void *d, unsigned w, unsigned h, size_t p)
{ (void)d; (void)w; (void)h; (void)p; }
static void b_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t b_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void b_poll(void) { }
static int16_t b_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)port; (void)dev; (void)idx; (void)id; return 0; }

uint16_t fe_gbadual_keys_from_retro(uint32_t pad)
{
   static const unsigned rp[10] = {
      RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L };
   uint16_t k = 0;
   unsigned i;
   for (i = 0; i < 10; i++)
      if (pad & (1u << rp[i]))
         k |= (uint16_t)(1u << i);
   return k;
}

/* ---- instance B's static state belongs to the session (section 19) ----
 *
 * B's .bss is one block, rounded up to a 32 KiB ROM page (psp/
 * gbalink_core_b.ld).  While B is not loaded it is LENT to instance A's ROM
 * cache as a second spare pool, so a single-player game keeps those
 * megabytes: whole MiB as blocks, the remainder as the cache's last, partial
 * block (gba_memory.c gamepak_buffer_pages).  Before B is loaded
 * the block is zeroed and B's .data is put back from the copy taken at boot,
 * so every session's instance B starts from exactly the bytes a fresh boot
 * gives it, however the last session or the lending left them. */
static int g_bpristine;

void fe_gbadual_boot(void)
{
   size_t n;
   if (g_bpristine || !gbab_bdata_start || !gbab_bdata_pristine)
      return;
   n = (size_t)(gbab_bdata_end - gbab_bdata_start);
   memcpy(gbab_bdata_pristine, gbab_bdata_start, n);
   g_bpristine = 1;
}

size_t fe_gbadual_lendable(void)
{
   if (!gbab_bregion_start || !gbab_bregion_lend)
      return 0;
   return (size_t)(gbab_bregion_lend - gbab_bregion_start);
}

void fe_gbadual_lend(int on)
{
   size_t n = fe_gbadual_lendable();
   gamepak_spare_pool2 = on && n ? (uint8_t *)gbab_bregion_start : NULL;
   gamepak_spare_pool2_bytes = on && n ? (uint32_t)n : 0u;
}

int fe_gbadual_lent(void)
{
   return gamepak_spare_pool2 != NULL;
}

/* B fresh: never while the block is lent or A's ROM cache still holds
 * blocks carved from it (the caller unloaded A first). */
static void b_fresh(void)
{
   size_t n;
   if (!gbab_bregion_start || !g_bpristine)
      return;
   n = (size_t)(gbab_bdata_end - gbab_bdata_start);
   memcpy(gbab_bdata_start, gbab_bdata_pristine, n);
   memset(gbab_bregion_start, 0, fe_gbadual_lendable());
}

int fe_gbadual_load(const fe_gbadual_config *cfg)
{
   struct retro_game_info gi;
   FILE *f;
   long size = 0;
   char msg[256];

   if (fe_gbadual_lent())
   {
      lg("gbadual_load FAILED reason=lent (B's state is A's ROM cache)");
      return -1;
   }
   b_fresh();
   memset(&D, 0, sizeof(D));
   D.cfg = *cfg;
#if defined(PSP)
   gbab_platform_cache_paranoid = platform_cache_paranoid;
#endif

   if (!cfg->own_rom)
   {
      /* B borrows A's ROM pool; A's page evictions unmap in B too. */
      gamepak_share_export(&D.share);
      gbab_gamepak_share_in = &D.share;
      gamepak_evict_hook = gbab_gamepak_share_unmap;
   }
   /* Own copy: B's ROM cache is exactly the cartridge (instance A already
    * took what it wanted; B must not take "blocks until malloc fails"). */
   else if ((f = fopen(cfg->rom_path, "rb")))
   {
      fseek(f, 0, SEEK_END);
      size = ftell(f);
      fclose(f);
   }
   gbab_gamepak_buffer_cap = (uint32_t)((size + (1 << 20) - 1) >> 20);
   if (!gbab_gamepak_buffer_cap)
      gbab_gamepak_buffer_cap = 1;
#if defined(PSP)
   gbab_jit_cache_force_small = 1;
   /* GBA-LINK-MIRROR.md 17.5: B's code away from A's in the I-cache.  Weak:
    * only a LAYOUT_PIN link core defines it. */
   if (&gbab_jit_pin_at)
      gbab_jit_pin_at = cfg->b_jit_pin_p1 ? (int32_t)(cfg->b_jit_pin_p1 - 1u)
                                          : -1;
#endif
   gbalink_headless_audio = gbab_gbalink_headless_audio =
      cfg->partner_audio > 0 ? 0u : (cfg->partner_audio < 0 ? 2u : 1u);
   D.sram_crc = cfg->sram_crc;
   if (&sched_fast && &gbab_sched_fast)
      gbab_sched_fast = sched_fast;
   /* a guest-visible switch: both machines of a pair must agree (and both
    * consoles of a session: fe_gblink's core mask) */
   if (&timer_lazy && &gbab_timer_lazy)
      gbab_timer_lazy = timer_lazy;
   /* read by B's emitter when it builds its stubs (gbab_retro_load_game) */
   if (&irq_io_fast && &gbab_irq_io_fast)
      gbab_irq_io_fast = irq_io_fast;
   /* MSR_FAST builds: B's mode switches follow the player's switch */
   if (&msr_fast && &gbab_msr_fast)
      gbab_msr_fast = msr_fast;
   /* Section 21: the cable fast path, the same on both machines. */
   gbalink_fast = gbab_gbalink_fast =
      (cfg->cable_fast >= 0 && cfg->cable_fast <= 3) ? (uint32_t)cfg->cable_fast
                                                     : 0u;

   gbab_retro_set_environment(b_env);
   gbab_retro_set_video_refresh(b_video);
   gbab_retro_set_audio_sample(b_sample);
   gbab_retro_set_audio_sample_batch(b_batch);
   gbab_retro_set_input_poll(b_poll);
   gbab_retro_set_input_state(b_state);
   gbab_retro_init();
   D.inited = 1;
   memset(&gi, 0, sizeof(gi));
   gi.path = cfg->rom_path;
   if (!gbab_retro_load_game(&gi))
   {
      snprintf(msg, sizeof(msg), "gbadual_load FAILED rom=%s cap=%u",
               cfg->rom_path, (unsigned)gbab_gamepak_buffer_cap);
      lg(msg);
      D.failed = 1;
      return -1;
   }
   if (cfg->autopilot && cfg->autopilot[0])
   {
      D.use_ap = gbab_fe_autopilot_load(cfg->autopilot) == 0;
      snprintf(msg, sizeof(msg), "gbadual_autopilot path=%s ok=%d",
               cfg->autopilot, D.use_ap);
      lg(msg);
   }
   else if (cfg->script && cfg->script[0] &&
       gbalink_script_load(&D.script, cfg->script, SCRIPT_CAP) != 0)
   {
      snprintf(msg, sizeof(msg), "gbadual_script FAILED path=%s", cfg->script);
      lg(msg);
   }
   if (cfg->hash_path && cfg->hash_path[0])
      D.hash = fopen(cfg->hash_path, "w");
   gbalink_inst_get_ops(&D.ops[0]);
   gbab_gbalink_inst_get_ops(&D.ops[1]);
   b_ops_for_ap = &D.ops[1];
   D.ops[0].name = "A";
   D.ops[1].name = "B";
   D.loaded = 1;
   snprintf(msg, sizeof(msg),
            "gbadual_load ok rom=%s shared=%d swap=%d cap=%u script=%s "
            "frames=%u mirror=%d", cfg->rom_path, !cfg->own_rom,
            D.share.must_swap, (unsigned)gbab_gamepak_buffer_cap,
            cfg->script ? cfg->script : "-", D.script.n, cfg->mirror);
   lg(msg);
#if defined(PSP)
   /* where B's code landed (mod 8 KiB is what the I-cache sees) */
   snprintf(msg, sizeof(msg), "gbadual_jit b_base=%p mod8k=0x%04x stubs=%u "
            "pin_req=%d", (void *)gbab_rom_translation_cache,
            (unsigned)((uintptr_t)gbab_rom_translation_cache & 8191u),
            (unsigned)gbab_rom_cache_watermark,
            &gbab_jit_pin_at ? (int)gbab_jit_pin_at : -2);
   lg(msg);
#endif
   return 0;
}

/* gbalink_pair_config.input, slot 1 with its autopilot: run the engine for
 * the frame the partner is about to start (it reads the RAM the last one
 * left, as the harness does for the player). */
static int b_input(void *user, unsigned slot, uint32_t frame, uint16_t *keys)
{
   (void)user; (void)frame;
   if (slot != 1 || !D.use_ap)
      return 0;
   gbab_fe_autopilot_frame();
   *keys = fe_gbadual_keys_from_retro(b_ap_mask);
   return 1;
}

static void on_frame(void *user, unsigned slot, uint32_t frame)
{
   uint32_t h[8];
   (void)user;
   if (!D.hash)
      return;
   D.pair.m[slot].hash(h);
   fprintf(D.hash, "s=%u f=%u r=%08x i=%08x e=%08x io=%08x p=%08x o=%08x "
           "v=%08x\n", slot, (unsigned)frame, (unsigned)h[0], (unsigned)h[1],
           (unsigned)h[2], (unsigned)h[3], (unsigned)h[4], (unsigned)h[5],
           (unsigned)h[6]);
   if ((frame & 127) == 0)
      fflush(D.hash);
}

int fe_gbadual_state_loaded(const void *state, size_t n)
{
   gbalink_pair_config pc;
   char msg[160];
   void *sa, *sb;
   if (!D.loaded)
      return -1;
   if (D.cfg.session)
      return 0;            /* a session starts the pair (fe_gbadual_machines) */
   if (D.active)
      gbalink_pair_end(&D.pair);
   D.active = 0;
#if defined(PSP)
   gbab_platform_cache_paranoid = platform_cache_paranoid;
#endif
   if (n != gbab_retro_serialize_size() || !gbab_retro_unserialize(state, n))
   {
      snprintf(msg, sizeof(msg), "gbadual_state FAILED n=%u want=%u",
               (unsigned)n, (unsigned)gbab_retro_serialize_size());
      lg(msg);
      D.failed = 1;
      return -1;
   }
   /* Same battery image on both (the bench stages one save). */
   sa = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   sb = gbab_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   if (sa && sb)
      memcpy(sb, sa, 0x20000);
   memset(&pc, 0, sizeof(pc));
   pc.lookahead = D.cfg.lookahead;
   pc.local_slot = 0;
   pc.headless[1] = 1;
   pc.on_frame = on_frame;
   if (D.use_ap)
      pc.input = b_input;
   pair_guard_cfg(&pc);
   g_fault = 0;
   gbalink_pair_init(&D.pair, &D.ops[0], &D.ops[1], &pc);
   D.fed1 = 0;
   D.frames_local = 0;
   D.active = 1;
   snprintf(msg, sizeof(msg), "gbadual_start lookahead=%u margin=%u fast=%u/%u",
            (unsigned)D.pair.cfg.lookahead, (unsigned)D.pair.cfg.child_margin,
            (unsigned)gbalink_fast, (unsigned)gbab_gbalink_fast);
   lg(msg);
   return 0;
}

int fe_gbadual_active(void) { return D.active && !D.failed; }

/* DRPROF_HW (link profile): which instance the pair is running right now --
 * 0 = A (the frontend's, drawn), 1 = B (headless), -1 = neither (the pair's
 * own loop).  Read racily from the sampler thread; a torn read only
 * misfiles one sample. */
int fe_gbadual_running_inst(void)
{
   int s = D.pair.running;
   if (!D.active || s < 0)
      return -1;
   if (!D.sess_input)
      return s == 0 ? 0 : 1;          /* one-console bench: A is slot 0 */
   return s == D.local_slot ? 0 : 1;
}

int fe_gbadual_frame(uint16_t local_keys)
{
   gbalink_pair *p = &D.pair;
   uint32_t f0 = p->frames[0];
   int r;
   if (!fe_gbadual_active())
      return -1;
   gbalink_pair_input(p, 0, f0, local_keys);
   D.local_ring[f0 % GBALINK_IN_RING] = local_keys;
   /* The partner trails the player (gbalink_pair.h), so its inputs up to
    * the player's frame are known: the player's own (mirror) or the
    * script's, which is known in advance. */
   if (D.use_ap)
      ;                 /* slot 1 asks b_input when it starts a frame */
   else if (D.cfg.mirror)
      for (; D.fed1 <= f0; D.fed1++)
         gbalink_pair_input(p, 1, D.fed1,
                            D.local_ring[D.fed1 % GBALINK_IN_RING]);
   else
      for (; D.fed1 <= f0 + 64; D.fed1++)
         gbalink_pair_input(p, 1, D.fed1,
                            gbalink_script_keys(&D.script, D.fed1));
   r = gbalink_pair_step(p);
   if (r != GBALINK_PAIR_FRAME)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "gbadual_step FAILED r=%d err=%d f=%u/%u "
               "step_runs=%u idle=%u", r, p->error, (unsigned)p->frames[0],
               (unsigned)p->frames[1], (unsigned)p->step_runs,
               (unsigned)p->st.idle_runs);
      lg(msg);
      gbadual_stop("step");
      return -1;
   }
   /* A derailed machine (BADJUMP_SAFE) ends the session: the partner's
    * flag is ours to read (nobody else does); the player's we only look at
    * -- the frontend still clears it and resets the player's machine, which
    * must not happen while that machine is in cable mode. */
   if (&gbab_badjump_reset_pending && gbab_badjump_reset_pending)
   {
      char msg[160];
      gbab_badjump_reset_pending = 0;
      snprintf(msg, sizeof(msg), "gbadual_guest_fault slot=1 n=%u pc=%08x "
               "f=%u/%u", GBAD_U(gbab_badjump_recoveries),
               GBAD_U(gbab_badjump_last_pc), (unsigned)p->frames[0],
               (unsigned)p->frames[1]);
      lg(msg);
      gbadual_stop("partner_fault");
      return -1;
   }
   if (&badjump_reset_pending && badjump_reset_pending)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "gbadual_guest_fault slot=0 n=%u pc=%08x "
               "f=%u/%u", GBAD_U(badjump_recoveries), GBAD_U(badjump_last_pc),
               (unsigned)p->frames[0], (unsigned)p->frames[1]);
      lg(msg);
      gbadual_stop("player_fault");
      return 0;          /* the frame did complete */
   }
   D.frames_local++;
   return 0;
}

/* ---- the two-console session's machines (fe_gblink.h) ----------------------
 *
 * Slot 0 is the host's machine on BOTH consoles, so the pair -- and every
 * hash -- is the same computation on both.  Instance A (the frontend's core,
 * drawn and heard) is this console's machine: slot 0 on the host, slot 1 on
 * the join; instance B is the partner's, headless.  Which instance holds
 * which slot is hash-identical (desktop `--swap`, section 4). */

static gbalink_inst_ops *slot_ops(int slot)
{
   return &D.ops[slot == D.local_slot ? 0 : 1];
}

static void *slot_sram(int slot)
{
   return slot == D.local_slot ? retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)
                               : gbab_retro_get_memory_data(
                                    RETRO_MEMORY_SAVE_RAM);
}

static void sess_on_frame(void *user, unsigned slot, uint32_t frame)
{
   on_frame(user, slot, frame);          /* gba_dual_hash, if configured */
   if (D.sess_hook)
      D.sess_hook(D.sess_hook_user, (int)slot, frame);
}

static int sess_input(void *user, unsigned slot, uint32_t frame,
                      uint16_t *keys)
{
   (void)user;
   return D.sess_input &&
          D.sess_input(D.sess_input_user, (int)slot, frame, keys) ? 1 : 0;
}

/* Battery RAM is committed only after BOTH states have loaded. A rejected
 * partner must not replace the player's battery image during failed setup. */
static int sess_load(int slot, const uint8_t *st, size_t stn)
{
   int a = slot == D.local_slot;
   size_t want = a ? retro_serialize_size() : gbab_retro_serialize_size();
   char msg[160];
   if (!st || stn != want ||
       !(a ? retro_unserialize(st, stn) : gbab_retro_unserialize(st, stn)))
   {
      snprintf(msg, sizeof(msg), "gbadual_session_state FAILED slot=%d "
               "inst=%s n=%u want=%u", slot, a ? "A" : "B", (unsigned)stn,
               (unsigned)want);
      lg(msg);
      return -1;
   }
   return 0;
}

/* The cartridge clock (gba_memory.c rtc_current_time) is the state's base +
 * frames, so both consoles agree -- except when a state's base is not a real
 * date: then each core re-anchors it to ITS wall clock (gpsp_wallclock; B has
 * none, so uptime), and the two consoles would compute different machines.
 * For the session both cores read the session's clock (the host's, from
 * CONFIG); the player's hook is restored at the end. */
extern time_t (*gpsp_wallclock)(void);
extern time_t (*gbab_gpsp_wallclock)(void) GBAD_WEAK;
/* The player's cartridge clock is the console's wall clock (rtc_clock.h); a
 * session's is base + frames on BOTH consoles, as it always was.  Core B
 * never has a wall clock published, so only A's switch is needed. */
extern int gpsp_rtc_frames_only;
static time_t sess_clock_seed;
static time_t (*sess_saved_wallclock)(void);
static time_t (*sess_saved_b_wallclock)(void);
static int sess_saved_frames_only;
static int sess_clock_on;
static time_t sess_clock(void) { return sess_clock_seed; }

static void sess_clock_restore(void)
{
   if (!sess_clock_on)
      return;
   gpsp_wallclock = sess_saved_wallclock;
   gpsp_rtc_frames_only = sess_saved_frames_only;
   if (&gbab_gpsp_wallclock)
      gbab_gpsp_wallclock = sess_saved_b_wallclock;
   sess_clock_on = 0;
}

static int m_start(void *user, int local_slot, unsigned batch,
                   int64_t clock_seed,
                   const uint8_t *const state[2], const size_t state_n[2],
                   const uint8_t *const save[2], const size_t save_n[2],
                   void (*hook)(void *, int, uint64_t), void *hook_user)
{
   gbalink_pair_config pc;
   char msg[200];
   int slot;
   (void)user;
   if (!D.loaded || D.failed)
      return -1;
   /* Reject obvious payload errors before changing either core or clock. */
   for (slot = 0; slot < 2; slot++)
      if (!state[slot] || state_n[slot] != (slot == !!local_slot
             ? retro_serialize_size() : gbab_retro_serialize_size()) ||
          save_n[slot] > 0x20000 || (save_n[slot] && !save[slot]))
         return -1;
   if (D.active)
      gbalink_pair_end(&D.pair);
   D.active = 0;
   D.local_slot = local_slot ? 1 : 0;
#if defined(PSP)
   gbab_platform_cache_paranoid = platform_cache_paranoid;
#endif
   if (!sess_clock_on)
   {
      sess_saved_wallclock = gpsp_wallclock;
      sess_saved_frames_only = gpsp_rtc_frames_only;
      sess_saved_b_wallclock = &gbab_gpsp_wallclock ? gbab_gpsp_wallclock : NULL;
   }
   sess_clock_seed = (time_t)clock_seed;
   sess_clock_on = 1;
   gpsp_wallclock = sess_clock;
   gpsp_rtc_frames_only = 1;
   if (&gbab_gpsp_wallclock)
      gbab_gpsp_wallclock = sess_clock;
   /* The partner is disposable. Validate/load it before touching the local
    * machine, independent of which network slot the player occupies. */
   for (slot = 0; slot < 2; slot++)
   {
      int at = D.local_slot ^ (slot == 0);
      if (sess_load(at, state[at], state_n[at]) != 0)
      {
         sess_clock_restore();
         return -1;
      }
   }
   for (slot = 0; slot < 2; slot++)
   {
      void *sram = slot_sram(slot);
      if (sram && save_n[slot])
         memcpy(sram, save[slot], save_n[slot]);
   }
   memset(&pc, 0, sizeof(pc));
   /* CONFIG's batch: the lookahead in scanlines (the host decides). */
   pc.lookahead = batch ? (uint64_t)batch * 1232u : 0;
   pc.local_slot = (unsigned)D.local_slot;
   pc.headless[D.local_slot ^ 1] = 1;
   pc.on_frame = sess_on_frame;
   pc.input = sess_input;
   pair_guard_cfg(&pc);
   g_fault = 0;
   D.sess_hook = hook;
   D.sess_hook_user = hook_user;
   gbalink_pair_init(&D.pair, slot_ops(0), slot_ops(1), &pc);
   D.fed1 = 0;
   D.frames_local = 0;
   D.active = 1;
   snprintf(msg, sizeof(msg), "gbadual_session_start local_slot=%d "
            "lookahead=%u margin=%u state=%u/%u save=%u/%u fast=%u/%u",
            D.local_slot,
            (unsigned)D.pair.cfg.lookahead, (unsigned)D.pair.cfg.child_margin,
            (unsigned)state_n[0], (unsigned)state_n[1], (unsigned)save_n[0],
            (unsigned)save_n[1], (unsigned)gbalink_fast,
            (unsigned)gbab_gbalink_fast);
   lg(msg);
   return 0;
}

static int m_advance(void *user,
                     int (*input)(void *, int, uint64_t, uint16_t *),
                     void *input_user)
{
   gbalink_pair *p = &D.pair;
   int r;
   (void)user;
   if (!D.active || D.failed)
      return -1;
   D.sess_input = input;
   D.sess_input_user = input_user;
   r = gbalink_pair_step(p);
   if (r == GBALINK_PAIR_STALL)
      return 0;
   if (r != GBALINK_PAIR_FRAME)
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "gbadual_step FAILED r=%d err=%d f=%u/%u "
               "step_runs=%u idle=%u", r, p->error, (unsigned)p->frames[0],
               (unsigned)p->frames[1], (unsigned)p->step_runs,
               (unsigned)p->st.idle_runs);
      lg(msg);
      gbadual_stop("session_step");
      return -1;
   }
   /* Either machine derailed (BADJUMP_SAFE): the session ends.  Both
    * consoles derail at the same point, or the hashes say they did not. */
   if ((&gbab_badjump_reset_pending && gbab_badjump_reset_pending) ||
       (&badjump_reset_pending && badjump_reset_pending))
   {
      char msg[160];
      snprintf(msg, sizeof(msg), "gbadual_guest_fault A=%u B=%u pc=%08x/%08x "
               "f=%u/%u",
               &badjump_reset_pending ? (unsigned)badjump_reset_pending : 0u,
               &gbab_badjump_reset_pending ?
                  (unsigned)gbab_badjump_reset_pending : 0u,
               GBAD_U(badjump_last_pc), GBAD_U(gbab_badjump_last_pc),
               (unsigned)p->frames[0], (unsigned)p->frames[1]);
      lg(msg);
      if (&gbab_badjump_reset_pending)
         gbab_badjump_reset_pending = 0;
      gbadual_stop("session_guest_fault");
      return -1;
   }
   D.frames_local++;
   return 1;
}

static uint64_t m_sync_hash(void *user)
{
   uint32_t h0[8], h1[8];
   (void)user;
   if (!D.active)
      return 0;
   D.pair.m[0].hash(h0);
   D.pair.m[1].hash(h1);
   return ((uint64_t)h0[7] << 32) | h1[7];
}

/* The session's "is the game saving" watch (fe_gblink.c, every 30 slot-0
 * frames: an end is deferred while the battery is changing).  Default: the
 * machine's count of backup-chip writes (gba_memory.c backup_writes) -- free,
 * and exact about activity.  D.sram_crc (harness/config gba_link_sram_crc
 * = 1): the old FNV over the whole 128 KiB image, a 4-5 ms spike on a
 * PSP-1000.  Either is the same computation on both consoles. */
static uint32_t m_cart_crc(void *user, int slot)
{
   const uint32_t *w = (const uint32_t *)slot_sram(slot);
   uint32_t h = 2166136261u;
   unsigned i;
   (void)user;
   if (!D.sram_crc)
      return (slot == D.local_slot ? backup_writes : gbab_backup_writes) *
             2654435761u + 1u;
   if (!w)
      return 0;
   for (i = 0; i < 0x20000 / 4; i++)
      h = (h ^ w[i]) * 16777619u;
   return h;
}

static size_t m_save_size(void *user, int slot)
{
   (void)user;
   return slot_sram(slot) ? 0x20000 : 0;
}

static int m_save_read(void *user, int slot, void *buf, size_t n)
{
   const void *sram = slot_sram(slot);
   (void)user;
   if (!sram || n > 0x20000)
      return -1;
   memcpy(buf, sram, n);
   return 0;
}

static size_t m_state_size(void *user, int slot)
{
   (void)user;
   return slot == D.local_slot ? retro_serialize_size()
                               : gbab_retro_serialize_size();
}

static int m_state_save(void *user, int slot, void *buf, size_t n)
{
   (void)user;
   if (n != m_state_size(NULL, slot))
      return -1;
   return (slot == D.local_slot ? retro_serialize(buf, n)
                                : gbab_retro_serialize(buf, n)) ? 0 : -1;
}

/* m_stop's synthetic partner: no buttons (see gbadual_finish_local). */
static int finish_keys(void *u, int slot, uint64_t frame, uint16_t *keys)
{
   (void)u; (void)slot; (void)frame;
   *keys = 0;
   return 1;
}

/* LOST LINK: EACH PLAYER CARRIES ON (owner, 2026-10-03; docs/GBA-LINK-UI.md).
 * The player's machine (instance A) is their own live game, so a session
 * that ends without an agreed end does not rewind it: it continues from
 * where it stands.  But a pair step can return mid-frame for the local
 * machine (the partner stalled on a late input after A had run part of its
 * next frame), and a gpSP core can only leave cable mode and run on alone
 * from a frame boundary.  So the pair runs on until the local machine
 * completes its frame, the partner fed no buttons: on the player's screen
 * that is the cable pulled a few scanlines later than the radio lost it,
 * and the game's own link-error handling takes it from there.  At most a
 * frame of A; nothing runs when A is already at a boundary.  On an agreed
 * end the frontend restores the captured final state anyway. */
static void gbadual_finish_local(void)
{
   gbalink_pair *p = &D.pair;
   unsigned i;
   int r = 0;
   if (!D.active || D.failed || p->error ||
       p->m[D.local_slot].need_begin())
      return;
   D.sess_hook = NULL;                     /* the session is going away */
   D.sess_input = finish_keys;
   D.sess_input_user = NULL;
   for (i = 0; i < 16; i++)
   {
      r = gbalink_pair_step(p);
      if (r != GBALINK_PAIR_STALL)
         break;
   }
   {
      char msg[96];
      snprintf(msg, sizeof(msg), "gbadual_finish_local r=%d steps=%u f=%u/%u",
               r, i + 1, (unsigned)p->frames[0], (unsigned)p->frames[1]);
      lg(msg);
   }
}

static void m_stop(void *user)
{
   char msg[160];
   (void)user;
   gbadual_finish_local();
   snprintf(msg, sizeof(msg), "gbadual_session_stop frames=%u/%u switches=%u "
            "err=%d", (unsigned)D.pair.frames[0], (unsigned)D.pair.frames[1],
            (unsigned)D.pair.st.switches, D.pair.error);
   lg(msg);
   if (D.hash)
      fflush(D.hash);        /* the run may end without fe_gbadual_close */
   if (D.active)
      gbalink_pair_end(&D.pair);
   D.active = 0;
   D.sess_hook = NULL;
   D.sess_input = NULL;
   sess_clock_restore();
}

static fe_gblink_machines gbadual_m;

const fe_gblink_machines *fe_gbadual_machines(void)
{
   if (!D.loaded || !D.cfg.session)
      return NULL;
   memset(&gbadual_m, 0, sizeof(gbadual_m));
   gbadual_m.console = FE_CONSOLE_GBA;
   gbadual_m.header_off = 0xA0;          /* title, code, maker, ... */
   gbadual_m.rom_min = 0xC0;
   gbadual_m.rom_max = 32u * 1024 * 1024;
   gbadual_m.save_max = 0x20000;
   gbadual_m.start = m_start;
   gbadual_m.advance = m_advance;
   gbadual_m.sync_hash = m_sync_hash;
   gbadual_m.cart_crc = m_cart_crc;
   gbadual_m.save_size = m_save_size;
   gbadual_m.save_read = m_save_read;
   gbadual_m.state_size = m_state_size;
   gbadual_m.state_save = m_state_save;
   gbadual_m.stop = m_stop;
   return &gbadual_m;
}

void fe_gbadual_stats_evt(void (*evt)(const char *fmt, ...))
{
   gbalink_inst_stats s0, s1;
   gbalink_bus_stats bs;
   /* The bench (not a session) with no pair running is ONE machine: say so
    * on every heartbeat, or its core_prof reads as a pair's (section 21:
    * that is how a solo Street Fighter run was taken for the pair's cost). */
   if (!D.active && D.loaded && !D.cfg.session)
      evt("gbadual active=0 failed=%d (the pair is NOT running: one machine; "
          "it starts at a state load or with gba_dual_cold = 1)", D.failed);
   if (!D.active)
      return;
   D.pair.m[0].get_stats(&s0);
   D.pair.m[1].get_stats(&s1);
   gbalink_bus_get_stats(&bs);
   if (D.use_ap)
      evt("gbadual_autopilot status=%d", gbab_fe_autopilot_status());
   evt("gbadual frames=%u/%u switches=%u runs=%u/%u blocked=%u/%u "
       "mp=%u/%u n=%u/%u ev_in=%u/%u late=%u/%u late_max=%u/%u bus=%u/%u "
       "depth=%u ovf=%u pages=%u/%u",
       (unsigned)D.pair.frames[0], (unsigned)D.pair.frames[1],
       (unsigned)D.pair.st.switches, (unsigned)D.pair.st.runs[0],
       (unsigned)D.pair.st.runs[1], (unsigned)D.pair.st.blocked[0],
       (unsigned)D.pair.st.blocked[1], (unsigned)s0.mp_xfers,
       (unsigned)s1.mp_xfers, (unsigned)s0.n_xfers, (unsigned)s1.n_xfers,
       (unsigned)s0.ev_in, (unsigned)s1.ev_in, (unsigned)s0.ev_late,
       (unsigned)s1.ev_late, (unsigned)s0.late_max, (unsigned)s1.late_max,
       (unsigned)bs.posted[0], (unsigned)bs.posted[1],
       (unsigned)bs.depth_max, (unsigned)bs.overflow,
       (unsigned)gamepak_page_loads, (unsigned)gbab_gamepak_page_loads);
   /* Section 22: the fast path's checking mode (gba_link_fast = 2) and the
    * progress guard's high-water mark. */
   if (gbalink_fast == 2 || s0.fast_mismatch || s1.fast_mismatch)
      evt("gbalink_fast_check mode=%u checks=%u/%u mismatch=%u/%u "
          "run_calls_max=%u/%u", (unsigned)gbalink_fast,
          (unsigned)s0.fast_checks, (unsigned)s1.fast_checks,
          (unsigned)s0.fast_mismatch, (unsigned)s1.fast_mismatch,
          (unsigned)s0.run_calls_max, (unsigned)s1.run_calls_max);
   if (s0.fast_mismatch)
      evt("gbalink_fast_mismatch slot=0 n=%u where=%u cached=%llu true=%llu "
          "now=%llu", (unsigned)s0.fast_mismatch, (unsigned)s0.fm_where,
          (unsigned long long)s0.fm_cached, (unsigned long long)s0.fm_true,
          (unsigned long long)s0.fm_now);
   if (s1.fast_mismatch)
      evt("gbalink_fast_mismatch slot=1 n=%u where=%u cached=%llu true=%llu "
          "now=%llu", (unsigned)s1.fast_mismatch, (unsigned)s1.fm_where,
          (unsigned long long)s1.fm_cached, (unsigned long long)s1.fm_true,
          (unsigned long long)s1.fm_now);
   /* Hang study: progress guard, both machines' derails and flushes, and
    * the kernel cache calls each core made on the firmware's large-range
    * paths (I >= 16 KiB, D >= 64 KiB). */
   evt("gbadual_health idle=%u step_runs_max=%u peer_max=%u "
       "badjump=%u/%u rom_flush=%u/%u ram_flush=%u/%u "
       "big_i=%u/%u big_i_max=%u/%u big_d=%u/%u paranoid=%u/%u",
       (unsigned)D.pair.st.idle_runs, (unsigned)D.pair.st.step_runs_max,
       (unsigned)D.pair.st.step_peer_frames_max,
       GBAD_U(badjump_recoveries), GBAD_U(gbab_badjump_recoveries),
       GBAD_U(flush_rom_total), GBAD_U(gbab_flush_rom_total),
       GBAD_U(flush_ram_total), GBAD_U(gbab_flush_ram_total),
       GBAD_U(jit_big_i_calls), GBAD_U(gbab_jit_big_i_calls),
       GBAD_U(jit_big_i_max), GBAD_U(gbab_jit_big_i_max),
       GBAD_U(jit_big_d_calls), GBAD_U(gbab_jit_big_d_calls),
#if defined(PSP)
       (unsigned)platform_cache_paranoid, (unsigned)gbab_platform_cache_paranoid
#else
       0u, 0u
#endif
       );
}

/* For the watchdog thread (heartbeat / stall record): where the pair is.
 * Racy reads by design -- a thread that may be looking at a hung one. */
int fe_gbadual_probe(char *buf, size_t n)
{
   if (!D.loaded)
      return snprintf(buf, n, "pair=off err=%d", g_fault);
   {
      /* fm: the checking mode's disagreements (slot 0/slot 1), w/c/t the
       * first one's place, cached and true time on the slot that had it --
       * here because the heartbeat file is written when the log ring is not */
      gbalink_inst_stats s0, s1;
      const gbalink_inst_stats *m;
      memset(&s0, 0, sizeof(s0));
      memset(&s1, 0, sizeof(s1));
      if (D.active)
      {
         D.pair.m[0].get_stats(&s0);
         D.pair.m[1].get_stats(&s1);
      }
      m = s0.fast_mismatch ? &s0 : &s1;
      return snprintf(buf, n, "pair=%s run=%d step_runs=%u f=%u/%u sw=%u "
                      "bpc=%08x bbj=%u err=%d fast=%u fm=%u/%u w=%u c=%llu "
                      "t=%llu",
                      D.active ? "on" : (D.failed ? "failed" : "idle"),
                      (int)D.pair.running, (unsigned)D.pair.step_runs,
                      (unsigned)D.pair.frames[0], (unsigned)D.pair.frames[1],
                      (unsigned)D.pair.st.switches,
                      &gbab_reg ? (unsigned)gbab_reg[15] : 0u,
                      GBAD_U(gbab_badjump_recoveries), g_fault,
                      (unsigned)gbalink_fast,
                      (unsigned)s0.fast_mismatch, (unsigned)s1.fast_mismatch,
                      (unsigned)m->fm_where,
                      (unsigned long long)m->fm_cached,
                      (unsigned long long)m->fm_true);
   }
}

int fe_gbadual_loaded(void)
{
   return D.inited;
}

/* Section 19: instance B gives back everything it took -- its core's heap
 * allocations (frame and audio buffers, its private ROM page 0), its pair
 * and script -- so the session's end leaves the heap as the session's start
 * found it.  B's static state is then free to be lent again. */
void fe_gbadual_unload(void)
{
   int inited = D.inited, loaded = D.loaded;
   fe_gbadual_close();
   if (inited)
   {
      if (loaded)
         gbab_retro_unload_game();
      gbab_retro_deinit();
      if (gbab_dynarec_release_translation_caches)
         gbab_dynarec_release_translation_caches();
      if (gbab_gamepak_share_release)
         gbab_gamepak_share_release();
      /* B's XS_HEAP tables (cpu_threaded.c): their pointer is in B's .bss,
       * which b_fresh zeroes and the lend hands to A's ROM cache, so the
       * block is freed here or orphaned every session (70,672 B a cycle). */
      if (gbab_dynarec_heap_tables_release)
         gbab_dynarec_heap_tables_release();
   }
   gbab_gamepak_share_in = NULL;
   gamepak_evict_hook = NULL;
   memset(&D, 0, sizeof(D));
   lg("gbadual_unload");
}

void fe_gbadual_close(void)
{
   sess_clock_restore();
   gamepak_evict_hook = NULL;
   if (D.active)
      gbalink_pair_end(&D.pair);
   D.active = 0;
   if (D.hash)
      fclose(D.hash);
   D.hash = NULL;
   gbalink_script_free(&D.script);
}

#endif /* GBA_LINK */
