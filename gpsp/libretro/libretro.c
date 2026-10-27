

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <streams/file_stream.h>
#include <compat/strl.h>
#include <libretro.h>

#include "../common.h"
#include "../memmap.h"
#include "libretro_core_options.h"

#include "../gba_memory.h"
#include "../gba_cc_lut.h"

#if defined(VITA) && defined(HAVE_DYNAREC)
#include <psp2/kernel/sysmem.h>
static int translation_caches_inited = 0;
static inline int align(int x, int n) {
  return (((x >> n) + 1) << n );
}

#define FOUR_KB_ALIGN(x) align(x, 12)
#define MB_ALIGN(x) align(x, 20)

int _newlib_vm_size_user = ROM_TRANSLATION_CACHE_SIZE +
                           RAM_TRANSLATION_CACHE_SIZE; 

int getVMBlock();

#endif

#if defined(_3DS)
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#if defined(HAVE_DYNAREC)
#include <malloc.h>
#include "3ds/3ds_utils.h"
#define MEMOP_PROT   6
#define MEMOP_MAP 4
#define MEMOP_UNMAP 5
int32_t svcDuplicateHandle(uint32_t* out, uint32_t original);
int32_t svcCloseHandle(uint32_t handle);
int32_t svcControlProcessMemory(uint32_t process, void* addr0, void* addr1, uint32_t size, uint32_t type, uint32_t perm);
static int translation_caches_inited = 0;
#endif
#endif

#ifndef MAX_PATH
#define MAX_PATH (512)
#endif

// Usually 59.72750057 Hz, unless GBC_RATE is overclocked (for 60FPS)
#define GBA_FPS ((float) GBC_BASE_RATE) / (308 * 228 * 4)

static s16 *audio_sample_buffer        = NULL;
static float audio_samples_per_frame   = 0.0f;
static float audio_samples_accumulator = 0.0f;

/* Apply the gpsp_sound_rate core option. reinit_env is non-NULL only for
 * mid-session changes (retro_run variable check), where issuing
 * RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO is legal; at load time the new rate
 * is picked up by the normal av_info and buffer init paths. */
static void gpsp_apply_sound_rate(const char *value, retro_environment_t reinit_env)
{
   u32 new_bits = (value && !strcmp(value, "32768")) ? 15 : 16;

   if (new_bits == sound_freq_bits)
      return;

   sound_freq_bits = new_bits;
   sound_frequency = 1u << new_bits;
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;

   if (reinit_env)
   {
      struct retro_system_av_info av_info;
      render_gbc_sound();          /* drain pending ticks at the old rate */
      sound_frequency_changed();   /* recompute all derived steps */
      sound_flush_ring();
      retro_get_system_av_info(&av_info);
      reinit_env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
   }
}

/* Workaround for a RetroArch audio driver
 * limitation: a maximum of 1024 frames
 * can be written per call of audio_batch_cb() */
#define AUDIO_BATCH_FRAMES_MAX 1024

/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 30

u32 skip_next_frame                          = 0;
/* Frontend-forced frameskip: -1 = none (the frameskip policy decides), 0 = draw,
 * 1 = skip.  Set before retro_run by GBAdhoc's fast-forward draw gate
 * (docs/FF-PRESETS.md); it never changes emulation, only whether this frame is
 * rendered/captured -- exactly what the policies above already do. */
int frontend_skip_override                  = -1;
static frameskip_type current_frameskip_type = no_frameskip;
static u32 frameskip_threshold               = 0;
static u32 frameskip_interval                = 0;
static u32 frameskip_counter                 = 0;
static bool audio_buff_active                = false;
static unsigned audio_buff_occupancy         = 0;
static bool audio_buff_underrun              = false;
static unsigned audio_latency                = 0;
static bool update_audio_latency             = false;
static bios_type selected_bios               = auto_detect;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_environment_t environ_cb;
static retro_set_rumble_state_t rumble_cb;

struct retro_perf_callback perf_cb;

int dynarec_enable;
#if defined(HAVE_DYNAREC) && (defined(MMAP_JIT_CACHE) || defined(_3DS) || defined(VITA))
static bool dynarec_cache_ready = false;
#else
static bool dynarec_cache_ready = true;
#endif

bool gpsp_dynarec_cache_available(void)
{
   return dynarec_cache_ready;
}
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

static int rtc_mode = FEAT_AUTODETECT;
static int rumble_mode = FEAT_AUTODETECT;
static int serial_setting = SERIAL_MODE_AUTO;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static u16 *gba_screen_pixels_prev = NULL;
static u16 *gba_processed_pixels   = NULL;

static void (*video_post_process)(void) = NULL;
static bool post_process_cc  = false;
static bool post_process_mix = false;

static void error_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[gpSP]: %s\n", text);
}

static void info_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[gpSP]: %s\n", text);
}

static void show_warning_message(const char* text, unsigned durationms) {
  unsigned ifversion = 0;
  if (!environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &ifversion) || ifversion >= 1) {
    /* Use the new API to display messages */
    struct retro_message_ext msg = {
      .msg = text, .duration = durationms,
      .priority = 2, .level = RETRO_LOG_WARN,
      .target = RETRO_MESSAGE_TARGET_ALL,
      .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
      .progress = -1,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
  }
  else {
    struct retro_message msg = {.msg = text, .frames = durationms / 17};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
  }
}

/* Frameskip START */

static void audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   audio_buff_active    = active;
   audio_buff_occupancy = occupancy;
   audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (current_frameskip_type == no_frameskip)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }
   else
   {
      bool calculate_audio_latency = true;

      /* Both deterministic modes are open-loop — they ignore buffer status,
       * so do not ask the frontend for it. */
      if (current_frameskip_type == fixed_interval_frameskip ||
          current_frameskip_type == sparse_interval_frameskip)
         environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      else
      {
         struct retro_audio_buffer_status_callback buff_status_cb;
         buff_status_cb.callback = audio_buff_status_cb;

         if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &buff_status_cb))
         {
            error_msg("Frameskip disabled - frontend does not support audio buffer status monitoring");

            audio_buff_active       = false;
            audio_buff_occupancy    = 0;
            audio_buff_underrun     = false;
            audio_latency           = 0;
            calculate_audio_latency = false;
         }
      }

      if (calculate_audio_latency)
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / ((float) GBA_FPS);

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }

   }

   update_audio_latency = true;
   frameskip_counter    = 0;
}

/* Frameskip END */

/* Video post processing START */

/* Note: This code is intentionally W.E.T.
 * (Write Everything Twice). These functions
 * are performance critical, and we cannot
 * afford to do unnecessary comparisons/switches
 * inside the inner for loops */

static void video_post_process_cc(void)
{
   const uint16_t *src = gba_screen_pixels;
   uint16_t       *dst = gba_processed_pixels;
   const size_t    npx = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t src_color = src[i];

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((src_color & 0xFFC0) >> 1) | (src_color & 0x1F)];
   }
}

static void video_post_process_mix(void)
{
   /* Flat loop: pitch equals width so the row-stride bookkeeping in
    * the original two-level form was bookkeeping for no actual stride
    * mismatch.  Compilers can vectorize the flat loop more aggressively;
    * on ARM Cortex-A it makes a measurable difference at -O2.  After the
    * mix we used to memcpy(prev, curr, 75 KB) so the next frame's mix
    * could read the previous frame; that copy is now elided by swapping
    * the two buffer pointers - after the swap, gba_screen_pixels points
    * to what was the prev buffer (about to be overwritten by the next
    * scanline-render pass), and gba_screen_pixels_prev points at the
    * just-rendered frame.  Saves ~4.5 MB/s of memory bandwidth at 60 fps
    * with mix enabled. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      /* Mix colours:
       *   http://blargg.8bitalley.com/info/rgb_mixing.html */
      dst[i] = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;
   }

   /* Swap curr/prev: the just-rendered frame becomes prev for next time,
    * and the previous prev (now stale and about to be overwritten by the
    * next render pass) becomes the new render target.  Equivalent to the
    * old 'memcpy(prev, curr, GBA_SCREEN_BUFFER_SIZE)' but without the
    * copy.  Safe because the scanline renderer fully overwrites every
    * pixel of gba_screen_pixels before any read (fill_line_background
    * runs first per scanline) - no read-before-write hazard on the
    * post-swap buffer. */
   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void video_post_process_cc_mix(void)
{
   /* See video_post_process_mix for the loop-flatten + buffer-swap
    * rationale.  Same shape, plus the gba_cc_lut colour-correction
    * lookup on the mixed value. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      uint16_t rgb_mix  = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((rgb_mix & 0xFFC0) >> 1) | (rgb_mix & 0x1F)];
   }

   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void init_post_processing(void)
{
   video_post_process = NULL;

   /* If post processing is disabled, return
    * immediately */
   if (!post_process_cc && !post_process_mix)
      return;

   /* Initialise output buffer, if required */
   if (!gba_processed_pixels &&
       (post_process_cc || post_process_mix))
   {
#ifdef _3DS
      gba_processed_pixels = (u16*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_processed_pixels = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

      if (!gba_processed_pixels)
         return;

      memset(gba_processed_pixels, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Initialise 'history' buffer, if required.  If this alloc fails we
    * must NOT fall through to the mix-variant assignment below: those
    * functions dereference gba_screen_pixels_prev unconditionally.
    * Leave the user setting (post_process_mix) intact - the failure
    * may be transient - but skip the function-pointer assignment for
    * the mix path so video_run keeps working. */
   if (!gba_screen_pixels_prev &&
       post_process_mix)
   {
      gba_screen_pixels_prev = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);

      if (!gba_screen_pixels_prev && log_cb)
         log_cb(RETRO_LOG_WARN,
                "Failed to allocate frame history buffer; disabling mix component of post-processing this run\n");
      else if (gba_screen_pixels_prev)
         memset(gba_screen_pixels_prev, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Assign post processing function.  Treat post_process_mix as
    * effectively false if the history buffer is missing. */
   {
      bool mix_active = post_process_mix && (gba_screen_pixels_prev != NULL);

      if (post_process_cc && mix_active)
         video_post_process = video_post_process_cc_mix;
      else if (post_process_cc)
         video_post_process = video_post_process_cc;
      else if (mix_active)
         video_post_process = video_post_process_mix;
   }
}

/* Video post processing END */

/* Fast forward override */
void set_fastforward_override(bool fastforward)
{
   struct retro_fastforwarding_override ff_override;

   if (!libretro_supports_ff_override)
      return;

   ff_override.ratio        = -1.0f;
   ff_override.notification = true;

   if (fastforward)
   {
      ff_override.fastforward    = true;
      ff_override.inhibit_toggle = true;
   }
   else
   {
      ff_override.fastforward    = false;
      ff_override.inhibit_toggle = false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
}

static void audio_run(void)
{
   s16 *audio_buffer_ptr;
   u32 samples_to_read;
   u32 samples_produced;

   /* If retro_init's malloc failed we still get called every frame;
    * skip the work in that case rather than write through NULL. */
   if (!audio_sample_buffer)
      return;

   /* audio_samples_per_frame is decimal;
    * get integer component */
   samples_to_read = (u32)audio_samples_per_frame;

   /* Account for fractional component */
   audio_samples_accumulator += audio_samples_per_frame -
         (float)samples_to_read;

   if (audio_samples_accumulator >= 1.0f)
   {
      samples_to_read++;
      audio_samples_accumulator -= 1.0f;
   }

   samples_produced = sound_read_samples(audio_sample_buffer, samples_to_read);

   /* Workaround for a RetroArch audio driver
    * limitation: a maximum of 1024 frames
    * can be written per call of audio_batch_cb(),
    * so we have to send samples in chunks */
   audio_buffer_ptr = audio_sample_buffer;
   while (samples_produced > 0)
   {
      u32 samples_to_write = (samples_produced > AUDIO_BATCH_FRAMES_MAX) ?
            AUDIO_BATCH_FRAMES_MAX : samples_produced;

      audio_batch_cb(audio_buffer_ptr, samples_to_write);

      samples_produced -= samples_to_write;
      audio_buffer_ptr += samples_to_write << 1;
   }
}

static void video_run(void)
{
   u16 *gba_screen_pixels_buf = gba_screen_pixels;

   if (skip_next_frame)
   {
      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
      return;
   }

   if (video_post_process)
   {
      video_post_process();
      gba_screen_pixels_buf = gba_processed_pixels;
   }

   video_cb(gba_screen_pixels_buf, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
}

// Netplay (Netpacket) interface

u32 netplay_num_clients = 0, netplay_client_id = 0;
static retro_netpacket_send_t netpacket_send_fn_ptr = NULL;
static retro_netpacket_poll_receive_t netpacket_pollrcv_fn_ptr = NULL;

#ifdef GBA_LINK
/* gbalink_core.c: the end of retro_run for a machine stepped by the link
 * pair -- its audio and picture (both are no-ops for the headless partner,
 * whose callbacks discard them and whose frames are skipped). */
void gbalink_lr_frame_end(void)
{
   audio_run();
   video_run();
}
#endif

void netpacket_poll_receive() {
  if (netpacket_pollrcv_fn_ptr)
    netpacket_pollrcv_fn_ptr();
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len) {
  // Force all packets to be flushed ASAP, to minimize latency.
  if (netpacket_send_fn_ptr)
    netpacket_send_fn_ptr(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, client_id);
}

/* ADR-0060: the SAME send, minus the reliability guarantee.
 *
 * Everything used to go out RELIABLE because there was no way to say otherwise,
 * and that is measurably expensive for one specific message. The client replies
 * to EVERY host packet with NET_RFU_CLIENT_ACK (rfu.c, "so they know we are
 * alive!"), so its reliable payload count is its own data PLUS one per host
 * send -- measured at 1.92x the host across 15 runs. Each of those 16-byte
 * liveness ACKs then takes a transmit-queue slot, takes an ARQ window slot, and
 * is itself RETRANSMITTED whenever its own acknowledgement is late. An
 * acknowledgement, carried by a channel that acknowledges.
 *
 * It does not need to be reliable. The host uses CLIENT_ACK for exactly one
 * thing -- clttl = 0 -- against a 240-frame (4 s) disconnect threshold, and
 * ordinary client data resets the same counter anyway. A dropped one costs
 * nothing; you would need 4 seconds of total client silence to matter.
 *
 * Control messages (CONNECT/DISCONNECT/NACK) must keep using netpacket_send.
 * This exists for liveness chatter only. */
void netpacket_send_unreliable(uint16_t client_id, const void *buf, size_t len) {
  if (netpacket_send_fn_ptr)
    netpacket_send_fn_ptr(RETRO_NETPACKET_UNRELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, client_id);
}

static void netpacket_start(uint16_t client_id, retro_netpacket_send_t send_fn, retro_netpacket_poll_receive_t poll_receive_fn) {
  netpacket_send_fn_ptr = send_fn;
  netpacket_pollrcv_fn_ptr = poll_receive_fn;
  netplay_num_clients = 0;
  netplay_client_id = client_id;
}

// Netplay session ends.
static void netpacket_stop() {
  netpacket_send_fn_ptr = NULL;
  netpacket_pollrcv_fn_ptr = NULL;
}

static void netpacket_receive(const void* buf, size_t len, uint16_t client_id) {
  switch (serial_mode) {
  case SERIAL_MODE_RFU:
    rfu_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_POKE:
    serialpoke_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_AW1:
  case SERIAL_MODE_SERIAL_AW2:
    serialaw_net_receive(buf, len, client_id);
    break;
  };
}

// Ensure we do not have too many clients for the type of connection used.
static bool netpacket_connected(uint16_t client_id) {
  const uint8_t maxpl[] = {
     0,                        // SERIAL_MODE_DISABLED
     0,                        // SERIAL_MODE_GBP
     MAX_RFU_NETPLAYERS,       // SERIAL_MODE_RFU
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_POKE
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW1
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW2
     0,                        // SERIAL_MODE_AUTO
  };
  const u32 max_clients = maxpl[serial_mode] - 1U;

  if (netplay_num_clients >= max_clients)
    return false;

  netplay_num_clients++;
  return true;
}

static void netpacket_disconnected(uint16_t client_id) {
  netplay_num_clients--;
}

const struct retro_netpacket_callback netpacket_iface = {
  netpacket_start,          /* start */
  netpacket_receive,        /* receive */
  netpacket_stop,           /* stop */
  NULL,                     /* poll */
  netpacket_connected,      /* connected */
  netpacket_disconnected,   /* disconnected */
  GPSP_NETPACKET_VERSION,   /* core version char* */
};


#ifdef PERF_TEST

extern struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(X) \
   static struct retro_perf_counter X = {#X}; \
   do { \
      if (!(X).registered) \
         perf_cb.perf_register(&(X)); \
   } while(0)

#define RETRO_PERFORMANCE_START(X) perf_cb.perf_start(&(X))
#define RETRO_PERFORMANCE_STOP(X) perf_cb.perf_stop(&(X))
#else
#define RETRO_PERFORMANCE_INIT(X)
#define RETRO_PERFORMANCE_START(X)
#define RETRO_PERFORMANCE_STOP(X)

#endif

void retro_get_system_info(struct retro_system_info* info)
{
   info->library_name = GPSP_NAME;
 #ifdef GIT_VERSION
   info->library_version = GPSP_VERSION "-" GIT_VERSION;
 #else
   info->library_version = GPSP_VERSION;
 #endif
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gba|bin|agb|gbz|u1" ;
}


void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = GBA_SCREEN_WIDTH;
   info->geometry.base_height = GBA_SCREEN_HEIGHT;
   info->geometry.max_width = GBA_SCREEN_WIDTH;
   info->geometry.max_height = GBA_SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 3.0f / 2.0f;
   info->timing.fps = ((float) GBA_FPS);
   info->timing.sample_rate = sound_frequency;
}

void retro_init(void)
{
   u32 audio_sample_buffer_size;
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;
   audio_sample_buffer_size  = (((u32)((float)(GBA_SOUND_FREQUENCY) / (float)(GBA_FPS))) + 1) * 2;
   audio_sample_buffer       = (s16*)malloc(audio_sample_buffer_size * sizeof(s16));
   if (!audio_sample_buffer)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR,
                "Failed to allocate audio sample buffer (%u bytes)\n",
                (unsigned)(audio_sample_buffer_size * sizeof(s16)));
      /* Continue: audio_run() now guards against this NULL and the
       * frontend just runs silent.  Better than failing retro_init
       * outright on memory-constrained targets where a small audio
       * allocation may fail but the rest of the core still works. */
   }

#if defined(HAVE_DYNAREC)
  #if defined(MMAP_JIT_CACHE)
   rom_translation_cache = map_jit_block(ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
   if (rom_translation_cache)
   {
      ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
      dynarec_cache_ready = true;
   }
   else if (log_cb)
      log_cb(RETRO_LOG_WARN,
             "JIT memory unavailable; using the interpreter\n");
  #elif defined(_3DS)
   if (__ctr_svchax && !translation_caches_inited)
   {
      uint32_t currentHandle;
      check_rosalina();
      
      rom_translation_cache_ptr  = memalign(0x1000, ROM_TRANSLATION_CACHE_SIZE);
      ram_translation_cache_ptr  = memalign(0x1000, RAM_TRANSLATION_CACHE_SIZE);

      if (rom_translation_cache_ptr && ram_translation_cache_ptr)
      {
         int32_t dup_rc = svcDuplicateHandle(&currentHandle, 0xFFFF8001);
         int32_t rom_rc = -1, ram_rc = -1;
         if (dup_rc >= 0)
         {
            rom_rc = svcControlProcessMemory(currentHandle,
               rom_translation_cache, rom_translation_cache_ptr,
               ROM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
            if (rom_rc >= 0)
               ram_rc = svcControlProcessMemory(currentHandle,
                  ram_translation_cache, ram_translation_cache_ptr,
                  RAM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
            if (rom_rc >= 0 && ram_rc >= 0)
            {
               svcCloseHandle(currentHandle);
               rom_translation_ptr = rom_translation_cache;
               ram_translation_ptr = ram_translation_cache;
               ctr_flush_invalidate_cache();
               translation_caches_inited = 1;
               dynarec_cache_ready = true;
            }
            else
            {
               if (ram_rc >= 0)
                  svcControlProcessMemory(currentHandle, ram_translation_cache,
                     ram_translation_cache_ptr, RAM_TRANSLATION_CACHE_SIZE,
                     MEMOP_UNMAP, 0b111);
               if (rom_rc >= 0)
                  svcControlProcessMemory(currentHandle, rom_translation_cache,
                     rom_translation_cache_ptr, ROM_TRANSLATION_CACHE_SIZE,
                     MEMOP_UNMAP, 0b111);
               svcCloseHandle(currentHandle);
            }
         }
      }
      if (!dynarec_cache_ready && log_cb)
         log_cb(RETRO_LOG_WARN,
                "JIT memory unavailable; using the interpreter\n");
      if (!dynarec_cache_ready)
      {
         free(rom_translation_cache_ptr);
         free(ram_translation_cache_ptr);
         rom_translation_cache_ptr = ram_translation_cache_ptr = NULL;
      }
   }
  #elif defined(VITA)
   if(!translation_caches_inited){
      void* currentHandle;

      sceBlock = getVMBlock();
      
      if (sceBlock < 0)
      {
        return;
      }

      // get base address
      int ret = sceKernelGetMemBlockBase(sceBlock, &currentHandle);
      if (ret < 0)
      {
        return;
      }

      rom_translation_cache  = (u8*)currentHandle;
      ram_translation_cache  = rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE;
      rom_translation_ptr = rom_translation_cache;
      ram_translation_ptr = ram_translation_cache;
      sceKernelOpenVMDomain();
      translation_caches_inited = 1;
      dynarec_cache_ready = true;
    }
  #elif defined(RUNTIME_JIT_CACHE)
   /* HERE, before init_gamepak_buffer: that loop takes 1 MiB blocks until
    * malloc fails, so after it there is nothing left to size a cache from.
    * Always leaves a usable cache (the static small tier at worst). */
   dynarec_select_translation_caches();
  #endif
#endif

   init_gamepak_buffer();
   init_sound();

   if(!gba_screen_pixels)
#ifdef _3DS
      gba_screen_pixels = (uint16_t*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_screen_pixels = (uint16_t*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

   /* gba_screen_pixels is dereferenced unconditionally by both the
    * renderer (writes from update_scanline) and the post-process /
    * video_cb hand-off in video_run.  A NULL here means we cannot
    * safely run a single frame.  Log loudly; load_game will return
    * the failure to the frontend so the core unloads cleanly. */
   if (!gba_screen_pixels && log_cb)
      log_cb(RETRO_LOG_ERROR,
             "Failed to allocate frame buffer (%u bytes); core will refuse to load content\n",
             (unsigned)GBA_SCREEN_BUFFER_SIZE);

   libretro_supports_bitmasks = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   libretro_supports_ff_override = false;
   if (environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, NULL))
      libretro_supports_ff_override = true;

   // This interface is actually optional
   environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&netpacket_iface);

   current_frameskip_type = no_frameskip;
   frameskip_threshold    = 0;
   frameskip_interval     = 0;
   frameskip_counter      = 0;
   audio_buff_active      = false;
   audio_buff_occupancy   = 0;
   audio_buff_underrun    = false;
   audio_latency          = 0;
   update_audio_latency   = false;
   selected_bios          = auto_detect;
   selected_boot_mode     = boot_game;
}

void retro_deinit(void)
{
   perf_cb.perf_log();
   memory_term();

#if defined(MMAP_JIT_CACHE) && defined(HAVE_DYNAREC)
   if (dynarec_cache_ready)
      unmap_jit_block(rom_translation_cache,
                      ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
   rom_translation_cache = ram_translation_cache = NULL;
   dynarec_cache_ready = false;
#endif
#if defined(_3DS) && defined(HAVE_DYNAREC)

   if (__ctr_svchax && translation_caches_inited)
   {
      uint32_t currentHandle;
      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      svcControlProcessMemory(currentHandle,
                              rom_translation_cache, rom_translation_cache_ptr,
                              ROM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcControlProcessMemory(currentHandle,
                              ram_translation_cache, ram_translation_cache_ptr,
                              RAM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcCloseHandle(currentHandle);
      free(rom_translation_cache_ptr);
      free(ram_translation_cache_ptr);
      translation_caches_inited = 0;
      dynarec_cache_ready = false;
   }
#endif

#if defined(VITA) && defined(HAVE_DYNAREC)
    if(translation_caches_inited){
        translation_caches_inited = 0;
        dynarec_cache_ready = false;
    }
#endif

#ifdef _3DS
   linearFree(gba_screen_pixels);
   if (gba_processed_pixels)
      linearFree(gba_processed_pixels);
#else
   free(gba_screen_pixels);
   if (gba_processed_pixels)
      free(gba_processed_pixels);
#endif
   if (gba_screen_pixels_prev)
      free(gba_screen_pixels_prev);

   gba_screen_pixels      = NULL;
   gba_processed_pixels   = NULL;
   gba_screen_pixels_prev = NULL;
   video_post_process     = NULL;
   post_process_cc        = false;
   post_process_mix       = false;

   if (audio_sample_buffer)
      free(audio_sample_buffer);

   audio_sample_buffer       = NULL;
   audio_samples_per_frame   = 0.0f;
   audio_samples_accumulator = 0.0f;
}

static retro_time_t retro_perf_dummy_get_time_usec() { return 0; }
static retro_perf_tick_t retro_perf_dummy_get_counter() { return 0; }
static uint64_t retro_perf_dummy_get_cpu_features() { return 0; }
static void retro_perf_dummy_log() {}
static void retro_perf_dummy_counter(struct retro_perf_counter *counter) {};

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   struct retro_vfs_interface_info vfs_iface_info;

   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   perf_cb = (struct retro_perf_callback){
      retro_perf_dummy_get_time_usec,
      retro_perf_dummy_get_counter,
      retro_perf_dummy_get_cpu_features,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_log,
   };
   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);

   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) { }

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset(void)
{
   reset_gba();
}

size_t retro_serialize_size(void)
{
   return GBA_STATE_MEM_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   memset (data,0, GBA_STATE_MEM_SIZE);
   gba_save_state(data);

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   return gba_load_state(data);
}

#ifdef REWIND
/* ---- rewind snapshot image (docs/REWIND.md) -------------------------------
 * The save-state image, whole, plus what a save state leaves out and a
 * rewind cannot: the fractional audio-frame accumulator of this file (without
 * it the frame after a restore hands out one sample more or fewer than it
 * did the first time), the in-flight part of the sound mix ring
 * (sound.h), and gbc_sound_wave_update: "the game wrote the wave RAM and the
 * mixer has not yet turned it into samples".  That flag is consumed by the
 * next mix, which with the sound switched off (every game's first seconds)
 * can be hundreds of frames away; a restore that lost it left the wave
 * channel with the samples of before the write until the game wrote the
 * wave RAM again (found by rewinding to the first frames after power-on,
 * which became reachable when a save write stopped ending the history).
 * Fixed size and fixed layout, so two images can be XORed. */
#define GPSP_REWIND_EXTRA  (8u + SOUND_REWIND_EXTRA_BYTES)

size_t gpsp_rewind_state_size(void);
bool gpsp_rewind_gather(void *data, size_t size);
bool gpsp_rewind_restore(const void *data, size_t size);
/* 1 = a restore flushes the RAM translation cache only (savestate.c);
 * 0 = what a state load flushes, ROM and RAM both.  The frontend sets it. */
u32 gpsp_rewind_light;
extern u32 gba_rewind_light_restore;

size_t gpsp_rewind_state_size(void)
{
   return GBA_STATE_MEM_SIZE + GPSP_REWIND_EXTRA;
}

/* Measurement (fe_host_rewind_opts.gather_prof): the three pieces of the
 * serialised gather, timed apart.  gpsp_rewind_gprof(out, clock) installs
 * the clock (NULL keeps it) and returns count / clear / save / sound totals
 * in microseconds, zeroing them. */
static uint64_t (*rw_gp_clock)(void);
static u32 rw_gp[4];

void gpsp_rewind_gprof(u32 out[4], uint64_t (*clock)(void))
{
   if (clock)
   {
      extern uint64_t (*gba_rewind_lclock)(void);
      rw_gp_clock = clock;
      gba_rewind_lclock = clock;
   }
   if (out)
   {
      memcpy(out, rw_gp, sizeof(rw_gp));
      memset(rw_gp, 0, sizeof(rw_gp));
   }
}

bool gpsp_rewind_gather(void *data, size_t size)
{
   u8 *x = (u8 *)data + GBA_STATE_MEM_SIZE;
   uint64_t t0 = 0, t1 = 0, t2 = 0;
   if (size != GBA_STATE_MEM_SIZE + GPSP_REWIND_EXTRA)
      return false;
   if (rw_gp_clock) t0 = rw_gp_clock();
   memset(data, 0, GBA_STATE_MEM_SIZE);
   if (rw_gp_clock) t1 = rw_gp_clock();
   gba_save_state(data);
   if (rw_gp_clock) t2 = rw_gp_clock();
   memset(x, 0, 8);
   memcpy(x, &audio_samples_accumulator, sizeof(audio_samples_accumulator));
   x[4] = (u8)(gbc_sound_wave_update != 0);
   sound_rewind_extra_save(x + 8);
   if (rw_gp_clock)
   {
      rw_gp[0]++;
      rw_gp[1] += (u32)(t1 - t0);
      rw_gp[2] += (u32)(t2 - t1);
      rw_gp[3] += (u32)(rw_gp_clock() - t2);
   }
   return true;
}

/* ---- the same state, in pieces (fe_rewind_span) ---------------------------
 * For a snapshot that copies nothing it does not have to: the six big
 * arrays are compared where they lie, so what is serialised is only the
 * REST of the save-state image -- everything outside those arrays' bytes,
 * a few KiB -- plus the sound in flight.
 *
 * The rest is produced by the unchanged save-state writers into a scratch
 * image with gba_rewind_skip_arrays set (gba_memory.h), then gathered from
 * the byte ranges between the arrays; a restore scatters it back and runs
 * the unchanged loader with the same flag.  The arrays' places in the image
 * are found once, from a real image, never assumed. */
typedef struct gpsp_rewind_arr
{
   u32 bytes;
   u8 *live;
} gpsp_rewind_arr;

#define RW_ARRAYS 6
static struct { u32 off, len; } rw_gap[RW_ARRAYS + 1];
static u32 rw_gaps, rw_rest_bytes;

u32  gpsp_rewind_layout(u8 *scratch, gpsp_rewind_arr arr[RW_ARRAYS]);
void gpsp_rewind_rest_gather(u8 *scratch, u8 *rest);
bool gpsp_rewind_rest_restore(u8 *scratch, const u8 *rest, int keep_ram_code);
int  gpsp_rewind_arrays_restore(const u8 *ref, const u8 *ewram256,
                                const u8 *vram96, u32 *copied);
void gpsp_rewind_map_peek(u8 *ewram256, u8 *vram96);
void gpsp_rewind_map_clean(void);
u32  gpsp_rewind_audio_bytes(void);
u32  gpsp_rewind_audio_gather(u8 *dst, u32 stale);
void gpsp_rewind_audio_restore(const u8 *src);
int  gpsp_rewind_take_dirty(u8 *ewram256, u8 *vram96);
void gpsp_rewind_gprof(u32 out[4], uint64_t (*clock)(void));

/* scratch: GBA_STATE_MEM_SIZE bytes, kept by the caller for the two calls
 * below.  Returns the size of the rest (a multiple of 4), 0 on failure. */
u32 gpsp_rewind_layout(u8 *scratch, gpsp_rewind_arr arr[RW_ARRAYS])
{
   static const char *name[RW_ARRAYS] =
      { "iwram", "ewram", "vram", "oamram", "palram", "ioregs" };
   u8 *live[RW_ARRAYS];
   const u8 *memdoc, *pad;
   u32 i, at = 0;

   live[0] = &iwram[0x8000];
   live[1] = ewram;
   live[2] = vram;
   live[3] = (u8 *)oam_ram;
   live[4] = (u8 *)palette_ram;
   live[5] = (u8 *)io_registers;

   memset(scratch, 0, GBA_STATE_MEM_SIZE);
   gba_rewind_skip_arrays = 0;
   gba_save_state(scratch);
   memdoc = bson_find_key(scratch, "memory");
   pad = bson_find_key(scratch, "zpadding");
   if (!memdoc || !pad)
      return 0;
   rw_gaps = 0;
   rw_rest_bytes = 0;
   for (i = 0; i < RW_ARRAYS; i++)
   {
      const u8 *p = bson_find_key(memdoc, name[i]);
      u32 off;
      if (!p)
         return 0;
      off = (u32)(p + 5 - scratch);        /* past the length and subtype */
      if (off < at)
         return 0;                         /* not in the order written */
      arr[i].bytes = bson_read_u32(p);
      arr[i].live = live[i];
      rw_gap[rw_gaps].off = at;
      rw_gap[rw_gaps].len = off - at;
      rw_rest_bytes += off - at;
      rw_gaps++;
      at = off + arr[i].bytes;
   }
   /* ...and from the last array up to the padding's bytes.  The padding
    * and the terminator after it never change. */
   rw_gap[rw_gaps].off = at;
   rw_gap[rw_gaps].len = (u32)(pad + 5 - scratch) - at;
   rw_rest_bytes += rw_gap[rw_gaps].len;
   rw_gaps++;
   /* + the accumulator and the wave-RAM flag */
   rw_rest_bytes = (rw_rest_bytes + 8 + 3) & ~3u;
   return rw_rest_bytes;
}

void gpsp_rewind_rest_gather(u8 *scratch, u8 *rest)
{
   u32 i, n = 0;
   gba_rewind_skip_arrays = 1;
   gba_save_state(scratch);
   gba_rewind_skip_arrays = 0;
   for (i = 0; i < rw_gaps; i++)
   {
      memcpy(rest + n, scratch + rw_gap[i].off, rw_gap[i].len);
      n += rw_gap[i].len;
   }
   memcpy(rest + n, &audio_samples_accumulator, 4);
   n += 4;
   rest[n++] = (u8)(gbc_sound_wave_update != 0);
   while (n < rw_rest_bytes)
      rest[n++] = 0;
}

/* The arrays must already be in place (the palette is converted and the
 * video counters reloaded from them). */
/* Measurement (gather_prof): of a restore's time outside the arrays, the
 * sound-ring clear and the state loader.  count / clear / load, zeroed. */
/* out: count, sound-ring clear, loader, copying the rest into the image,
 * then the loader's own five parts (savestate.c, gba_rewind_lprof). */
static u32 rw_rp[4];
void gpsp_rewind_rprof(u32 out[9]);
void gpsp_rewind_rprof(u32 out[9])
{
   extern u32 gba_rewind_lprof[5];
   memcpy(out, rw_rp, sizeof(rw_rp));
   memcpy(out + 4, gba_rewind_lprof, sizeof(gba_rewind_lprof));
   memset(rw_rp, 0, sizeof(rw_rp));
   memset(gba_rewind_lprof, 0, sizeof(gba_rewind_lprof));
}

bool gpsp_rewind_rest_restore(u8 *scratch, const u8 *rest, int keep_ram_code)
{
   u32 i, n = 0;
   uint64_t t0 = 0, t1 = 0, tc = 0;
   bool ok;
   if (rw_gp_clock) tc = rw_gp_clock();
   for (i = 0; i < rw_gaps; i++)
   {
      memcpy(scratch + rw_gap[i].off, rest + n, rw_gap[i].len);
      n += rw_gap[i].len;
   }
   /* The scratch may be a buffer shared with ordinary state saves: its last
    * byte is the document's terminator whoever wrote it last. */
   scratch[GBA_STATE_MEM_SIZE - 1] = 0;
   gba_rewind_skip_arrays = 1;
   gba_rewind_light_restore = !gpsp_rewind_light ? 0 : keep_ram_code ? 2 : 1;
   if (rw_gp_clock) t0 = rw_gp_clock();
   if (gpsp_rewind_light)
   {
      sound_rewind_clear_inflight();
      gba_rewind_ring_cleared = 1;
   }
   if (rw_gp_clock) t1 = rw_gp_clock();
   ok = gba_load_state(scratch);
   if (rw_gp_clock)
   {
      rw_rp[0]++;
      rw_rp[1] += (u32)(t1 - t0);
      rw_rp[2] += (u32)(rw_gp_clock() - t1);
      rw_rp[3] += (u32)(t0 - tc);
   }
   gba_rewind_ring_cleared = 0;
   gba_rewind_light_restore = 0;
   gba_rewind_skip_arrays = 0;
   if (ok)
   {
      memcpy(&audio_samples_accumulator, rest + n, 4);
      gbc_sound_wave_update = rest[n + 4];
   }
   return ok;
}

/* ---- the arrays back from the reference, only where they can differ ------
 * ref: the six arrays as they lie in the reference image (IWRAM, EWRAM,
 * VRAM, OAM, palette, I/O).  ewram256 / vram96: pages (1 KiB) that may
 * differ from the reference, NULL = all of them.  IWRAM and the three small
 * arrays are always copied whole.
 *
 * Returns 1 when a byte under translated RAM code changed -- then the RAM
 * translation cache has to go (gpsp_rewind_rest_restore, keep_ram_code = 0).
 * Code is only ever inside the tracked ranges (cpu_threaded.c), so only
 * what is copied there is compared first.  Driver 2 runs its engine from
 * EWRAM: its code pages do not change, and it no longer retranslates 280 KB
 * after every restore.
 *
 * VRAM pages copied are marked in vram_clean for the renderer, as a store
 * would have. */
#ifdef HAVE_DYNAREC
extern u32 iwram_code_min, iwram_code_max, ewram_code_min, ewram_code_max;
#endif

/* Does [off, off + len) of a RAM (live vs the reference) differ anywhere
 * under translated code?  tags: that RAM's code-tag half, same offsets (a
 * nonzero tag = an instruction translated from there).  Looked at a word at
 * a time: a word with any tag in it counts whole, which is at least as wide
 * as what the store stubs themselves check.  Only the tracked code range
 * [cmin, cmax) can hold tags at all. */
static int rw_code_differs(const u8 *live, const u8 *ref, const u8 *tags,
                           u32 off, u32 len, u32 cmin, u32 cmax, u32 size)
{
#ifdef HAVE_DYNAREC
   u32 lo, hi, i;
   if (!cmax)
      return 0;                         /* no translated code in this RAM */
   if (cmax > cmin)
   {
      lo = cmin & ~15u;
      hi = cmax + 8 < size ? cmax + 8 : size;
   }
   else
   {
      lo = 0;
      hi = size;
   }
   if (off + len <= lo || off >= hi)
      return 0;
   if (lo < off)
      lo = off;
   if (hi > off + len)
      hi = off + len;
   lo &= ~3u;
   hi = (hi + 3u) & ~3u;
   for (i = lo; i < hi; i += 4)
      if (*(const u32 *)(tags + i) &&
          *(const u32 *)(live + i) != *(const u32 *)(ref + i))
         return 1;
   return 0;
#else
   (void)live; (void)ref; (void)tags; (void)off; (void)len; (void)cmin;
   (void)cmax; (void)size;
   return 0;
#endif
}

int gpsp_rewind_arrays_restore(const u8 *ref, const u8 *ewram256,
                               const u8 *vram96, u32 *copied)
{
   int code = 0;
   u32 p, n = 0;
#ifdef HAVE_DYNAREC
   u32 icmin = iwram_code_min, icmax = iwram_code_max;
   u32 ecmin = ewram_code_min, ecmax = ewram_code_max;
#else
   u32 icmin = 0, icmax = 0, ecmin = 0, ecmax = 0;
#endif

   code |= rw_code_differs(&iwram[0x8000], ref, iwram, 0, 0x8000, icmin, icmax,
                           0x8000);
   memcpy(&iwram[0x8000], ref, 0x8000);
   ref += 0x8000;
   n += 0x8000;

   if (!ewram256)
   {
      code |= rw_code_differs(ewram, ref, ewram + 0x40000, 0, 0x40000, ecmin,
                              ecmax, 0x40000);
      memcpy(ewram, ref, 0x40000);
      n += 0x40000;
   }
   else
      for (p = 0; p < 256; p++)
         if (ewram256[p])
         {
            code |= rw_code_differs(ewram, ref, ewram + 0x40000, p << 10, 1024,
                                    ecmin, ecmax, 0x40000);
            memcpy(ewram + (p << 10), ref + (p << 10), 1024);
            n += 1024;
         }
   ref += 0x40000;

   if (!vram96)
   {
      memcpy(vram, ref, sizeof(vram));
      memset(vram_clean, 0, sizeof(vram_clean));
      n += sizeof(vram);
   }
   else
      for (p = 0; p < 96; p++)
         if (vram96[p])
         {
            memcpy(vram + (p << 10), ref + (p << 10), 1024);
            vram_clean[p] = 0;
            vram_clean[VRAM_DIRTY_ANY] = 0;
            VRAM_X_MARK(p << VRAM_DIRTY_SHIFT); /* VRAM_EXEC: may be new code */
            n += 1024;
         }
   ref += sizeof(vram);

   memcpy(oam_ram, ref, sizeof(oam_ram));
   ref += sizeof(oam_ram);
   memcpy(palette_ram, ref, sizeof(palette_ram));
   ref += sizeof(palette_ram);
   memcpy(io_registers, ref, sizeof(io_registers));
   n += sizeof(oam_ram) + sizeof(palette_ram) + sizeof(io_registers);
   if (copied)
      *copied = n;
   return code;
}

/* The written-page map as it stands, without taking it (1 = written). */
void gpsp_rewind_map_peek(u8 *ewram256, u8 *vram96)
{
   u32 p;
   for (p = 0; p < 256; p++)
      ewram256[p] = (u8)(!gba_rewind_map_ok ||
                         !gba_rewind_clean[RW_CLEAN_EWRAM + p]);
   for (p = 0; p < 96; p++)
      vram96[p] = (u8)(!gba_rewind_map_ok ||
                       !gba_rewind_clean[RW_CLEAN_VRAM + p]);
}

/* The machine was just made equal to the reference: nothing is written. */
void gpsp_rewind_map_clean(void)
{
   memset(gba_rewind_clean, 1, RW_CLEAN_BYTES);
}

void gpsp_rewind_marks_want(int on);
void gpsp_rewind_marks_want(int on)
{
   gba_rewind_marks_wanted = on ? 1u : 0u;
}

int gpsp_rewind_marks_ok(void);
int gpsp_rewind_marks_ok(void)
{
   return (int)gba_rewind_map_ok;
}

extern u32 gba_rewind_clock_frames;
void gpsp_rewind_clock_sync(void);
void gpsp_rewind_clock_mode(int frames);
void gpsp_rewind_clock_mode(int frames)
{
   /* leaving the mode: one last sync, so the wall clock takes over from
    * where the frame clock stood and never from behind it */
   if (!frames && gba_rewind_clock_frames)
      gpsp_rewind_clock_sync();
   gba_rewind_clock_frames = frames ? 1u : 0u;
}

u32 gpsp_rewind_audio_bytes(void)
{
   return SOUND_REWIND_EXTRA_BYTES;
}

u32 gpsp_rewind_audio_gather(u8 *dst, u32 stale)
{
   return sound_rewind_extra_save_part(dst, stale);
}

void gpsp_rewind_audio_restore(const u8 *src)
{
   sound_rewind_extra_load(src);
}

/* The rewind's written-page map (gba_memory.h, gba_rewind_clean), handed
 * over and started afresh: out[p] != 0 = page p (1 KiB) may have been
 * written since the last call.  0 = the map cannot be trusted (the dynarec
 * could not be given it): the outputs are untouched and the caller must
 * read everything. */
/* Somebody wrote guest memory past every store path (the frontend's
 * poke): no page can be called clean. */
void gpsp_rewind_mark_all(void);
void gpsp_rewind_mark_all(void)
{
   RW_MARK_ALL();
}

int gpsp_rewind_take_dirty(u8 *ewram256, u8 *vram96)
{
   u32 p;
   if (!gba_rewind_map_ok)
      return 0;
   for (p = 0; p < 256; p++)
      ewram256[p] = (u8)!gba_rewind_clean[RW_CLEAN_EWRAM + p];
   for (p = 0; p < 96; p++)
      vram96[p] = (u8)!gba_rewind_clean[RW_CLEAN_VRAM + p];
   memset(gba_rewind_clean, 1, RW_CLEAN_BYTES);
   return 1;
}

bool gpsp_rewind_restore(const void *data, size_t size)
{
   const u8 *x = (const u8 *)data + GBA_STATE_MEM_SIZE;
   if (size != GBA_STATE_MEM_SIZE + GPSP_REWIND_EXTRA)
      return false;
   gba_rewind_light_restore = gpsp_rewind_light;
   if (!gba_load_state(data))
   {
      gba_rewind_light_restore = 0;
      return false;
   }
   gba_rewind_light_restore = 0;
   memcpy(&audio_samples_accumulator, x, sizeof(audio_samples_accumulator));
   gbc_sound_wave_update = x[4];
   sound_rewind_extra_load(x + 8);
   return true;
}
#endif /* REWIND */

void retro_cheat_reset(void)
{
   cheat_clear();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   if (!enabled)
   {
      /* Per the libretro contract, retro_cheat_set with enabled=false
       * must deactivate the cheat at this index.  Without this, a user
       * toggling a cheat off in the frontend would have no effect:
       * the cheat would stay active until the next retro_cheat_reset()
       * or content load.  Note RetroArch typically calls
       * retro_cheat_reset followed by a full reapply on every change,
       * so the visible bug surface is small, but other frontends and
       * runtime API callers can hit this directly. */
      cheat_disable(index);
      return;
   }

   switch (cheat_parse(index, code))
   {
   case CheatErrorTooMany:
      show_warning_message("Too many active cheats!", 2500);
      break;
   case CheatErrorTooBig:
      show_warning_message("Cheats are too big!", 2500);
      break;
   case CheatErrorEncrypted:
      show_warning_message("Encrypted cheats are not supported!", 2500);
      break;
   case CheatErrorNotSupported:
      show_warning_message("Cheat type is not supported!", 2500);
      break;
   case CheatNoError:
      break;
#ifdef GPSP_CHEATS
   /* The strict parser's own reasons (cheats.h).  The PSP frontend never
    * comes through here (fe_cheats.c shows the reason on the Cheats page);
    * another libretro frontend gets the words. */
   case CheatErrorSyntax:
   case CheatErrorGameShark:
   case CheatErrorRegion:
   case CheatErrorAlign:
   case CheatErrorMixed:
   case CheatErrorUnsupported:
      show_warning_message("Cheat type is not supported!", 2500);
      break;
   case CheatErrorWork:
      show_warning_message("Cheats exceed the per-frame work limit!", 2500);
      break;
#endif
   };
}

static void extract_directory(char* buf, const char* path, size_t size)
{
   char* base;

   strlcpy(buf, path, size);

   base = strrchr(buf, '/');

   if (base)
      *base = '\0';
   else
      strlcpy(buf, ".", size);
}

static void check_variables(bool started_from_load)
{
   struct retro_variable var;
   bool frameskip_type_prev;
   bool post_process_cc_prev;
   bool post_process_mix_prev;

#ifdef HAVE_DYNAREC
   int dynarec_requested = 1;
   int dynarec_previous = dynarec_enable;
   var.key = "gpsp_drc";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         dynarec_requested = 0;
      else if (strcmp(var.value, "enabled") == 0)
         dynarec_requested = 1;
   }
   dynarec_enable = dynarec_requested && dynarec_cache_ready;
   if (dynarec_requested && !dynarec_cache_ready && log_cb)
      log_cb(RETRO_LOG_WARN,
             "Dynarec requested but its code cache is unavailable; using the interpreter\n");
   /* Flush only when the effective execution mode changes. */
   if (dynarec_cache_ready && dynarec_enable != dynarec_previous)
   {
      if (dynarec_enable)
         main_enable_dynarec();
      else
         flush_dynarec_caches();
   }
#else
   dynarec_enable = 0;
#endif

   if (started_from_load) {
     var.key                = "gpsp_bios";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "auto"))
           selected_bios = auto_detect;
        else if (!strcmp(var.value, "builtin"))
           selected_bios = builtin_bios;
        else if (!strcmp(var.value, "official"))
           selected_bios = official_bios;
     }

     var.key                = "gpsp_boot_mode";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "game"))
           selected_boot_mode = boot_game;
        else if (!strcmp(var.value, "bios"))
           selected_boot_mode = boot_bios;
     }

     var.key                = "gpsp_rtc";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rtc_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rtc_mode = FEAT_ENABLE;
        else
           rtc_mode = FEAT_AUTODETECT;
     }

     var.key                = "gpsp_serial";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           serial_setting = SERIAL_MODE_DISABLED;
        else if (!strcmp(var.value, "rfu"))
           serial_setting = SERIAL_MODE_RFU;
        else if (!strcmp(var.value, "mul_poke"))
           serial_setting = SERIAL_MODE_SERIAL_POKE;
        else if (!strcmp(var.value, "mul_aw1"))
           serial_setting = SERIAL_MODE_SERIAL_AW1;
        else if (!strcmp(var.value, "mul_aw2"))
           serial_setting = SERIAL_MODE_SERIAL_AW2;
        else if (!strcmp(var.value, "gbp"))
           serial_setting = SERIAL_MODE_GBP;
        else
           serial_setting = SERIAL_MODE_AUTO;
     }

     var.key                = "gpsp_rumble";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rumble_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rumble_mode = FEAT_ENABLE;
        else
           rumble_mode = FEAT_AUTODETECT;
     }
   }

   var.key                = "gpsp_sprlim";
   var.value              = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         sprite_limit = 1;
      else if (strcmp(var.value, "enabled") == 0)
         sprite_limit = 0;
   }
   {
      static int sound_rate_configured = 0;
      struct retro_variable svar = { "gpsp_sound_rate", NULL };
      environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &svar);
      gpsp_apply_sound_rate(svar.value, sound_rate_configured ? environ_cb : NULL);
      sound_rate_configured = 1;
   }


   var.key                = "gpsp_frameskip";
   var.value              = 0;
   frameskip_type_prev    = current_frameskip_type;
   current_frameskip_type = no_frameskip;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
         current_frameskip_type = auto_frameskip;
      else if (!strcmp(var.value, "auto_threshold"))
         current_frameskip_type = auto_threshold_frameskip;
      else if (!strcmp(var.value, "fixed_interval"))
         current_frameskip_type = fixed_interval_frameskip;
      else if (!strcmp(var.value, "sparse_interval"))
         current_frameskip_type = sparse_interval_frameskip;
   }

   var.key             = "gpsp_frameskip_threshold";
   var.value           = 0;
   frameskip_threshold = 33;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   var.key   = "gpsp_frameskip_interval";
   var.value = 0;

   frameskip_interval = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_interval = strtol(var.value, NULL, 10);

   /* (Re)Initialise frame skipping, if required */
   if (started_from_load ||
       (current_frameskip_type != frameskip_type_prev))
      init_frameskip();

   var.key              = "gpsp_color_correction";
   var.value            = NULL;
   post_process_cc_prev = post_process_cc;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_cc = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_cc = true;
   }

   var.key               = "gpsp_frame_mixing";
   var.value             = NULL;
   post_process_mix_prev = post_process_mix;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_mix = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_mix = true;
   }

   /* Check whether post processing options
    * have changed */
   if ((post_process_cc != post_process_cc_prev) ||
       (post_process_mix != post_process_mix_prev))
      init_post_processing();

   var.key           = "gpsp_turbo_period";
   var.value         = NULL;
   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      turbo_period = atoi(var.value);
      turbo_period = (turbo_period < TURBO_PERIOD_MIN) ?
            TURBO_PERIOD_MIN : turbo_period;
      turbo_period = (turbo_period > TURBO_PERIOD_MAX) ?
            TURBO_PERIOD_MAX : turbo_period;

      turbo_pulse_width = turbo_period >> 1;
      turbo_pulse_width = (turbo_pulse_width < TURBO_PULSE_WIDTH_MIN) ?
            TURBO_PULSE_WIDTH_MIN : turbo_pulse_width;
      turbo_pulse_width = (turbo_pulse_width > TURBO_PULSE_WIDTH_MAX) ?
            TURBO_PULSE_WIDTH_MAX : turbo_pulse_width;

      turbo_a_counter = 0;
      turbo_b_counter = 0;
   }
}

static void set_input_descriptors()
{
   struct retro_input_descriptor descriptors[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0 },
   };

   struct retro_input_descriptor descriptors_ff[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },
      { 0 },
   };

   if (libretro_supports_ff_override)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors_ff);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors);
}

static void set_memory_descriptors(void)
{
   const uint64_t mem = RETRO_MEMORY_SYSTEM_RAM;
   struct retro_memory_descriptor desc[2] = {
      { mem, iwram, 0x00000 + 0x8000, 0x3000000, 0, 0,  0x8000, NULL },
      { mem, ewram, 0x00000,          0x2000000, 0, 0, 0x40000, NULL },
   };
   struct retro_memory_map retromap = {
      desc,
      sizeof(desc) / sizeof(desc[0])
   };
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retromap);
}

bool retro_load_game(const struct retro_game_info* info)
{
   if (!info || !info->path)
      return false;

   /* If retro_init failed to allocate the frame buffer, refuse to load
    * rather than crash on the first video_run.  retro_init is allowed
    * to fail allocations silently; this is the point at which we have
    * to decide whether the core can usefully run. */
   if (!gba_screen_pixels)
      return false;

   check_variables(true);
   set_input_descriptors();

   char filename_bios[MAX_PATH];
   const char* dir = NULL;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      info_msg("RGB565 is not supported.");

   extract_directory(main_path, info->path, sizeof(main_path));

   bool bios_loaded = false;
   if (selected_bios == auto_detect || selected_bios == official_bios)
   {
     /* Build '<dir>/gba_bios.bin'.  strlcpy returns the source length,
      * so we can reuse it as the offset for the suffix append - one
      * less strlen call than a strlcat-style pattern, and bounded
      * against MAX_PATH overflow either way.  If the directory path
      * was already truncated (len >= sizeof(filename_bios)), the
      * subsequent load_bios will fail and we fall back to the
      * built-in BIOS - the right outcome for a pathological frontend
      * path. */
     const char *base_dir = (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
                            ? dir : main_path;
     size_t len = strlcpy(filename_bios, base_dir, sizeof(filename_bios));
     if (len < sizeof(filename_bios))
        strlcpy(filename_bios + len, "/gba_bios.bin", sizeof(filename_bios) - len);

     bios_loaded = true;

     if (load_bios(filename_bios) != 0)
     {
        if (selected_bios == official_bios)
          show_warning_message("Could not load BIOS image file, using built-in BIOS", 2500);
        bios_loaded = false;
     }

     if (bios_loaded && bios_rom[0] != 0x18)
     {
        if (selected_bios == official_bios)
          show_warning_message("BIOS image seems incorrect, using built-in BIOS", 2500);
        bios_loaded = false;
     }
   }

   if (!bios_loaded) {
     /* Load the built-in BIOS */
     memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

   memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
   if (load_gamepak(info, info->path, rtc_mode, rumble_mode, serial_setting) != 0)
   {
      error_msg("Could not load the game file.");
      return false;
   }

   if (selected_bios != builtin_bios && gamepak_header_nonstandard)
   {
      show_warning_message("Non-standard ROM header detected, using built-in BIOS", 2500);
      memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

   extern bool require_m1_hle_bios;

   if (selected_bios != builtin_bios && require_m1_hle_bios)
   {
      show_warning_message("Pokemon ROM Hack (M1) detected, forcing built-in BIOS", 2500);
      memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

#ifdef HAVE_DYNAREC
   if (gamepak_mini_materialized && dynarec_enable)
   {
      dynarec_enable = 0;
      flush_dynarec_caches();
      show_warning_message("Mini ROM detected, dynamic recompiler disabled", 2500);
   }
#endif

   struct retro_rumble_interface rumbleif;
   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumbleif))
      rumble_cb = rumbleif.set_rumble_state;
   else
      rumble_cb = NULL;

   reset_gba();

   set_memory_descriptors();

   return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info* info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
   memory_unload_gamepak();

   if (libretro_ff_enabled)
      set_fastforward_override(false);

   libretro_supports_bitmasks    = false;
   libretro_supports_ff_override = false;
   libretro_ff_enabled           = false;
   libretro_ff_enabled_prev      = false;

   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   turbo_a_counter   = 0;
   turbo_b_counter   = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return gamepak_backup;
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return ewram;

   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return 0x20000;  /* Assume 128KiB, biggest possible save */
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return 0x40000;

   return 0;
}

void retro_run(void)
{
   bool updated = false;
   /* Phase 5h (see main.h): bracket this call by phase.  Every one of these
    * is 0 when the profile is off, so the arithmetic below reduces to
    * `0 - 0` and the only cost is the assignments. */
   u32 cph_start, cph_t, cph_emu = 0, cph_aout = 0, cph_blt = 0, cph_rfu = 0;

   core_phase_frame_begin();
   cph_start = core_phase_now(CORE_PHASE_COARSE);

   input_poll_cb();
   update_input();

#ifdef GPSP_CHEATS
   /* The one fixed point codes run at (cheats.h): after this frame's input is
    * latched (CodeBreaker button tests read KEYINPUT), before the CPU runs.
    * No active cheat, or a link session => not even the call. */
   if (cheats_active && !cheats_suspended)
      cheats_apply_frame();
#endif

   rumble_frame_reset();

   /* Check whether current frame should
    * be skipped */
   skip_next_frame = 0;

   if (current_frameskip_type != no_frameskip)
   {
      switch (current_frameskip_type)
      {
         case auto_frameskip:

            skip_next_frame =
                  (audio_buff_active && audio_buff_underrun) ?
                        1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case auto_threshold_frameskip:

            skip_next_frame =
                  (audio_buff_active &&
                        (audio_buff_occupancy < frameskip_threshold)) ?
                              1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case fixed_interval_frameskip:

            if (frameskip_counter < frameskip_interval)
            {
               skip_next_frame   = 1;
               frameskip_counter++;
            }
            else
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }

            break;

         case sparse_interval_frameskip:

            /* Inverse of the above: draw `frameskip_interval` frames, then
             * skip exactly one.  interval 3 => draw,draw,draw,skip (75 % of
             * frames drawn), interval 7 => 87.5 %.  Interval 0 degenerates to
             * "skip every frame", so treat it as no skipping at all. */
            if (frameskip_interval == 0)
               skip_next_frame = 0;
            else if (frameskip_counter < frameskip_interval)
            {
               skip_next_frame   = 0;
               frameskip_counter++;
            }
            else
            {
               skip_next_frame   = 1;
               frameskip_counter = 0;
            }

            break;
         default:
            skip_next_frame = 0;
            break;
      }
   }

   if (frontend_skip_override >= 0)
      skip_next_frame = (u32)frontend_skip_override;

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   /* This runs just a frame */
   cph_t = core_phase_now(CORE_PHASE_COARSE);
   #ifdef HAVE_DYNAREC
   if (dynarec_enable)
   {
      execute_arm_translate(execute_cycles);
      DRPH_SET(DRPH_RETRO);
   }
   else
   #endif
   {
      /* Sticky bits only used in interpreter */
      clear_gamepak_stickybits();
      execute_arm(execute_cycles);
   }
   cph_emu = core_phase_now(CORE_PHASE_COARSE) - cph_t;

   if (rumble_cb) {
     // TODO: Add some user-option to select a rumble policy
     u32 strength = 0xffff * rumble_active_pct();
     rumble_cb(0, RETRO_RUMBLE_WEAK,   MIN(strength, 0xffff));
     rumble_cb(0, RETRO_RUMBLE_STRONG, MIN(strength, 0xffff) / 2);
   }

   cph_t = core_phase_now(CORE_PHASE_COARSE);
   audio_run();
   cph_aout = core_phase_now(CORE_PHASE_COARSE) - cph_t;

   cph_t = core_phase_now(CORE_PHASE_COARSE);
   video_run();
   cph_blt = core_phase_now(CORE_PHASE_COARSE) - cph_t;

   cph_t = core_phase_now(CORE_PHASE_COARSE);
   switch (serial_mode) {
   case SERIAL_MODE_RFU:
     rfu_frame_update();
     break;
   case SERIAL_MODE_SERIAL_POKE:
     serialpoke_frame_update();
     break;
   };
   cph_rfu = core_phase_now(CORE_PHASE_COARSE) - cph_t;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);

   core_phase_frame_end(core_phase_now(CORE_PHASE_COARSE) - cph_start,
                        cph_emu, cph_blt, cph_aout, cph_rfu);
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}
