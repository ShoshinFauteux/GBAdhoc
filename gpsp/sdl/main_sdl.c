/* main_sdl.c — SDL2 desktop twin of the PSP frontend (plan §4.2, Phase 1).
 *
 * Hosts the gpsp libretro core via frontend-common/fe_host and mirrors the
 * PSP frontend's behavior: RGB565 240x160, 59.7275 Hz pacing, EVT logging,
 * SRAM discipline, BMP frame dumps, --autoexit for automated runs.
 *
 * Usage:
 *   gpsp_sdl --rom FILE [--bios-dir DIR] [--save FILE] [--log FILE]
 *            [--autoexit N] [--dump-at N] [--dump-dir DIR] [--scale N]
 *            [--no-pacing] [--force-sram-write] [--option key=value]
 *            [--script FILE] [--ff]
 *            [--host [PORT]] [--join IP:PORT] [--nick NAME]
 *            [--net-loss PCT] [--net-jitter MS] [--net-probe]
 *
 * --script runs an autopilot input script (fe_autopilot.h grammar); the run
 * exits automatically when the script finishes (code 0) or fails (code 3).
 * --ff is uncapped fast-forward (harness mode, plan §7.1): no pacing, audio
 * muted, only every 32nd frame presented; scripts can also toggle it.
 *
 * Networking (Phase 3): --host binds a UDP session host on PORT (default
 * 19019 = 0x4A4B, plan §4.3), --join connects to one. netdrv bridges the
 * core's netpacket interface over UDP; --net-loss/--net-jitter engage the
 * transport fault shim (netsmoke hardening) and --net-probe the 1 Hz
 * reliable probe the smoke harness asserts on. FF is interlocked to 1x
 * while a session is active (plan §4.4).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <time.h>

#include <SDL.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_autopilot.h"
#include "netpacket_host.h"
#include "transport_udp.h"

#define NET_DEFAULT_PORT 19019   /* 0x4A4B */

static uint64_t net_now_us(void)
{
   /* CLOCK_MONOTONIC: netdrv keepalive/RTO timers must never see wall-clock
    * steps (NTP/hypervisor time sync). */
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}

/* --trace NAME:SZ:ADDR[:OFF] — sample RAM every 300 frames into EVT trace
 * lines (OFF given => ADDR holds a u32 GBA pointer, read at *ADDR+OFF).
 * Harness diagnostic for long interactive flows (Union Room states etc). */
#define TRACE_MAX 8
static struct { char name[16]; uint32_t addr, off; int size, deref; }
   g_trace[TRACE_MAX];
static int g_trace_n;

static int trace_add(const char *spec)
{
   char buf[128], *tok[4];
   int n = 0;
   char *p, *save = NULL;
   if (g_trace_n >= TRACE_MAX)
      return -1;
   strncpy(buf, spec, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';
   for (p = strtok_r(buf, ":", &save); p && n < 4; p = strtok_r(NULL, ":", &save))
      tok[n++] = p;
   if (n < 3)
      return -1;
   strncpy(g_trace[g_trace_n].name, tok[0], 15);
   g_trace[g_trace_n].size = (int)strtol(tok[1], NULL, 0);
   g_trace[g_trace_n].addr = (uint32_t)strtoul(tok[2], NULL, 0);
   g_trace[g_trace_n].deref = (n == 4);
   g_trace[g_trace_n].off = (n == 4) ? (uint32_t)strtoul(tok[3], NULL, 0) : 0;
   g_trace_n++;
   return 0;
}

static void trace_emit(void)
{
   char line[320];
   size_t used = 0;
   int i;
   if (!g_trace_n)
      return;
   line[0] = '\0';
   for (i = 0; i < g_trace_n; i++)
   {
      uint8_t b[4] = { 0, 0, 0, 0 };
      uint32_t addr = g_trace[i].addr;
      int ok = 1;
      if (g_trace[i].deref)
      {
         ok = fe_host_mem_read(addr, b, 4) == 0;
         addr = ((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)) + g_trace[i].off;
         memset(b, 0, 4);
      }
      if (ok)
         ok = fe_host_mem_read(addr, b, (unsigned)g_trace[i].size) == 0;
      used += (size_t)snprintf(line + used, sizeof(line) - used,
                               "%s%s=", i ? " " : "", g_trace[i].name);
      if (ok)
         used += (size_t)snprintf(line + used, sizeof(line) - used, "0x%x",
                                  (unsigned)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)));
      else
         used += (size_t)snprintf(line + used, sizeof(line) - used, "ERR");
      if (used >= sizeof(line) - 24)
         break;
   }
   fe_evt("trace frame=%u %s", fe_host_frame_count(), line);
}

#define FF_PRESENT_MASK 31   /* in FF, present every 32nd frame */

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;
static SDL_AudioDeviceID audio_dev;
static int have_video;
static int running = 1;
static int g_ff;             /* uncapped fast-forward active this frame */

/* Keep at most ~250 ms queued; beyond that we drop (pacing drift guard). */
#define AUDIO_QUEUE_MAX_BYTES (65536 / 4 * 4 * 2) /* 250ms * 4 bytes * stereo-ish */

static void plat_video_frame(const uint16_t *pix, unsigned w, unsigned h,
                             size_t pitch)
{
   (void)w; (void)h;   /* texture is fixed 240x160 */
   if (!have_video)
      return;
   if (g_ff && (fe_host_frame_count() & FF_PRESENT_MASK))
      return;   /* FF: blit only the latest frame at a low cadence */
   if (pix)
      SDL_UpdateTexture(texture, NULL, pix, (int)pitch);
   SDL_RenderClear(renderer);
   SDL_RenderCopy(renderer, texture, NULL, NULL);
   SDL_RenderPresent(renderer);
}

static void plat_audio_frames(const int16_t *lr, size_t frames)
{
   if (!audio_dev || g_ff)   /* FF mutes audio: queue drains to silence */
      return;
   if (SDL_GetQueuedAudioSize(audio_dev) > AUDIO_QUEUE_MAX_BYTES)
      return; /* drop: queue is far ahead of real time */
   SDL_QueueAudio(audio_dev, lr, (Uint32)(frames * 2 * sizeof(int16_t)));
}

/* RETRO_DEVICE_ID_JOYPAD_* without pulling all of libretro.h in here. */
#define RETRO_DEVICE_ID_JOYPAD_B      0
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START  3
#define RETRO_DEVICE_ID_JOYPAD_UP     4
#define RETRO_DEVICE_ID_JOYPAD_DOWN   5
#define RETRO_DEVICE_ID_JOYPAD_LEFT   6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT  7
#define RETRO_DEVICE_ID_JOYPAD_A      8
#define RETRO_DEVICE_ID_JOYPAD_L      10
#define RETRO_DEVICE_ID_JOYPAD_R      11

static uint32_t plat_input_bitmask(void)
{
   const Uint8 *k = SDL_GetKeyboardState(NULL);
   uint32_t m = 0;
   if (k[SDL_SCANCODE_UP])       m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
   if (k[SDL_SCANCODE_DOWN])     m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
   if (k[SDL_SCANCODE_LEFT])     m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
   if (k[SDL_SCANCODE_RIGHT])    m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
   if (k[SDL_SCANCODE_X])        m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
   if (k[SDL_SCANCODE_Z])        m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
   if (k[SDL_SCANCODE_A])        m |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
   if (k[SDL_SCANCODE_S])        m |= 1 << RETRO_DEVICE_ID_JOYPAD_R;
   if (k[SDL_SCANCODE_RETURN])   m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
   if (k[SDL_SCANCODE_RSHIFT])   m |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
   return m;
}

/* ---- RFU wall clock (ADR-0045) -----------------------------------------
 *
 * rfu.c declares this WEAK and defaults it to `return 0`, and only
 * psp/main_psp.c ever defined it strongly.  The desktop build therefore ran
 * with rfu_frame_us pinned at 0, which silently disabled three separate
 * mechanisms on this rig:
 *
 *   ADR-0055/0058  answer latency: the sample is gated on
 *                  `rfu_client.pkts[0].t_us != 0` and t_us is assigned from
 *                  rfu_frame_us, so it was never taken -- rfu_answer_census
 *                  reported n=0 forever, which is the "dead instrument"
 *                  reading that ADR-0058 exists to distinguish from "genuinely
 *                  sub-frame".
 *   ADR-0060       client keepalive: fires on
 *                  `rfu_frame_us - rfu_cli_last_tx_us >= 1 s`, i.e. 0 >= 1 s,
 *                  so the desktop client never sent one.
 *   ADR-0043       flight recorder: every timestamp in the ring was 0.
 *
 * Read once per emulated frame by rfu_frame_update(), never per command. */
unsigned gpsp_rfu_now_us(void);
unsigned gpsp_rfu_now_us(void)
{
   return (unsigned)net_now_us();
}

/* ---- RFU trace consumer (desktop twin of psp/main_psp.c's) --------------
 *
 * rfu.c raises gpsp_rfu_trace_hook() for every adapter state change, login
 * phase, answer-latency census, RESP_TIMEO and so on.  The symbol is WEAK and
 * only psp/main_psp.c ever defined it strongly, so until now the desktop
 * build linked the no-op default: every RFU probe in the codebase was silent
 * on this rig, and a desktop log could not be compared against a hardware log
 * on any of the quantities the field investigation turns on.  This is the
 * same drain as the PSP's, with fe_np_session_active() standing in for the
 * PSP's g_net_up.
 *
 * Single-threaded here (fe_host_run_frame() calls the core inline), so the
 * PSP's lock-free ring is not strictly needed -- but keeping the identical
 * shape means the two frontends' output is diffable line for line. */
#define RFU_TRACE_RING 64
static volatile unsigned g_rfu_tr_buf[RFU_TRACE_RING];
static volatile unsigned g_rfu_tr_head, g_rfu_tr_tail, g_rfu_tr_lost;

/* ADR-0075 smoothing valve — set the client's per-frame delivery admission so
 * a jittered link can be A/B'd on the desktop (--rfu-pace N; N=0 = off). */
extern void rfu_set_frame_pace(unsigned n);
extern void rfu_set_cushion(unsigned n);   /* fixed-depth jitter buffer PoC */

/* ME renderer capture validation (--me-capture-test): run the production
 * capture path in mode 2 (capture AND render) so an ME_CAP_VALIDATE build of
 * video.cc can replay-from-capture and prove the capture bit-exact.  Raw-byte
 * buffer sized to me_capture_frame (video.h): 160*64 u16 + 4 s32 + 1 u32. */
extern unsigned int me_capture_mode;
extern void *me_capture_buf;
static unsigned char g_mecap_test_buf[160*64*2 + 4*4 + 4]
   __attribute__((aligned(8)));

void gpsp_rfu_trace_hook(unsigned ev, unsigned a, unsigned b);
void gpsp_rfu_trace_hook(unsigned ev, unsigned a, unsigned b)
{
   unsigned head = g_rfu_tr_head;
   if (head - g_rfu_tr_tail >= RFU_TRACE_RING) { g_rfu_tr_lost++; return; }
   g_rfu_tr_buf[head % RFU_TRACE_RING] =
      ((ev & 0xFF) << 24) | ((a & 0xFFF) << 12) | (b & 0xFFF);
   g_rfu_tr_head = head + 1;
}

static const char *rfu_tr_state_name(unsigned s)
{
   static const char *n[] = { "idle", "host", "connecting", "client" };
   return s < 4 ? n[s] : "?";
}

static void rfu_trace_drain(void)
{
   static unsigned reported_lost;
   const char *net = fe_np_session_active() ? "up" : "down";

   while (g_rfu_tr_tail != g_rfu_tr_head)
   {
      unsigned v  = g_rfu_tr_buf[g_rfu_tr_tail % RFU_TRACE_RING];
      unsigned ev = (v >> 24) & 0xFF, a = (v >> 12) & 0xFFF, b = v & 0xFFF;
      g_rfu_tr_tail++;

      switch (ev)
      {
      case 1:
         fe_evt("rfu_cmderr cmd=0x%02x state=%s net=%s",
                a, rfu_tr_state_name(b), net);
         break;
      case 2:
         fe_evt("rfu_state new=%s cause=%u net=%s",
                rfu_tr_state_name(a), b, net);
         break;
      case 3:
         fe_evt("rfu_unkcmd cmd=0x%02x state=%s", a, rfu_tr_state_name(b));
         break;
      case 4:
         fe_evt("rfu_qdrop side=%s slot=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 6:
      {
         static const char *ph[] = { "reset", "handshake", "logged_in" };
         fe_evt("rfu_login phase=%s prev_words=%u", a < 3 ? ph[a] : "?", b);
         break;
      }
      case 7:
         fe_evt("rfu_rxburst frame_max=%u prev=%u net=%s", a, b, net);
         break;
      case 8:
         fe_evt("rfu_rxhold frame_max=%u delivered=%u", a, b);
         break;
      case 9:
         fe_evt("rfu_qhi side=%s depth=%u", a ? "host_rx" : "client_rx", b);
         break;
      case 10:
         fe_evt("rfu_answer max_us=%u mean_us=%u", a, b);
         break;
      case 11:
         fe_evt("rfu_answer_census n=%u max_us=%u", a, b);
         break;
      case 12:
         fe_evt("rfu_timeo n=%u t_frames=%u net=%s", a, b, net);
         break;
      case 13:
         fe_evt("rfu_syscfg timeout_frames=%u rtx_max=%u", a, b);
         break;
      case 14:
         fe_evt("rfu_noresp n16=%u rtx_max=%u", a, b);
         break;
      /* 15-19 mirror psp/main_psp.c's decode so a desktop A/B of the same
       * arm stays diffable line for line; see the long comments there. */
      case 15:
         fe_evt("rfu_rxgate n=%u/%u peak=%u run_max=%u net=%s",
                a & 0xFFF, 600u, 0u, b, net);
         break;
      case 16:
         fe_evt("rfu_discans slots=0x%x state=%u net=%s", a, b, net);
         break;
      case 17:
         fe_evt("rfu_arrival clumped=%u/%u net=%s", a, b, net);
         break;
      case 18:
         fe_evt("rfu_discq queued=%u mode=%u net=%s", a, b, net);
         break;
      case 19:
         fe_evt("rfu_pace n=%u/%u q_hi=%u net=%s", a, 600u, b, net);
         break;
      case 20:  /* ADR-0072 fix: game recvQueue depth peak (a) + over-gate frames (b) */
         fe_evt("rfu_gqpeak peak=%u over_gate=%u/%u net=%s", a, b, 600u, net);
         break;
      default:
         /* case 5 (RFU_TR_CMD) is per-command and would flood; ignored, the
          * same as the PSP frontend does unless explicitly enabled. */
         break;
      }
   }

   if (g_rfu_tr_lost != reported_lost)
   {
      reported_lost = g_rfu_tr_lost;
      fe_evt("rfu_trace_lost n=%u", reported_lost);
   }
}

/* ---- poll census: the desktop's `sess_cost poll=` --------------------------
 *
 * netpacket_poll_receive() is called from exactly one place in the core --
 * rfu_update()'s RFU_COMSTATE_WAITEVENT branch (rfu.c:1481-1501) -- so
 * poll_n per emulated frame IS the client's WAITEVENT dwell, expressed in
 * the same unit the hardware logs report it in.  Nothing on this rig calls
 * rfu_set_poll_min_cycles(), so rfu_poll_min_cycles is 0 and the count is
 * the UNLIMITED one: directly comparable to the hardware's pre-poll-limit
 * numbers (client ~431-495 vs host ~2.8-4.4 per frame), not the post-limit
 * 7-vs-1. */
#define SESS_POLL_WIN 600
static void sess_poll_evt(void)
{
   static fe_np_prof prev;
   static unsigned   prev_frame;
   static unsigned   next_at = SESS_POLL_WIN;
   fe_np_prof p;
   unsigned f;

   if (fe_host_frame_count() < next_at)
      return;
   f = fe_host_frame_count() - prev_frame;   /* EMULATED frames in the window:
                                              * fe_np_prof.frames is fed by
                                              * fe_np_prof_frame(), added below,
                                              * but the emulated count is the
                                              * denominator the hardware's
                                              * `sess_cost poll=` uses. */
   prev_frame = fe_host_frame_count();
   next_at    = fe_host_frame_count() + SESS_POLL_WIN;

   fe_np_prof_get(&p);
   if (!f) f = 1;
   fe_evt("sess_poll win=%u poll=%u.%02u work=%u.%02u poll_us=%u/%u "
          "frame_us=%u/%u pumps=%u net=%s",
          f,
          (p.poll_n - prev.poll_n) / f,
          (((p.poll_n - prev.poll_n) * 100u) / f) % 100u,
          (p.poll_work_n - prev.poll_work_n) / f,
          (((p.poll_work_n - prev.poll_work_n) * 100u) / f) % 100u,
          (p.poll_us - prev.poll_us) / f, p.poll_max_us,
          (p.frame_us - prev.frame_us) / f, p.frame_max_us,
          (p.pumps - prev.pumps) / f,
          fe_np_session_active() ? "up" : "down");
   prev = p;
}

int main(int argc, char **argv)
{
   const char *rom_path  = NULL;
   const char *bios_dir  = ".";
   const char *save_path = NULL;
   const char *log_path  = NULL;
   const char *dump_dir  = ".";
   const char *script    = NULL;
   long autoexit = 0, dump_at = 0, dump_every = 0;
   int scale = 2, no_pacing = 0, force_sram_write = 0, force_ff = 0;
   int rfu_pace = -1;   /* -1 = leave default; >=0 = rfu_set_frame_pace(n) */
   int rfu_cush = -1;   /* -1 = leave default; >=0 = rfu_set_cushion(n)    */
   int exit_code = 0;
   const char *exit_reason = NULL;
   char save_buf[512];
   fe_host_config cfg;
   int i;
   int net_host = 0, net_loss = 0, net_jitter = 0, net_probe = 0;
   long net_port = NET_DEFAULT_PORT;
   const char *net_join = NULL;     /* "ip:port" */
   const char *nick = "sdl";
   udp_transport *udp = NULL;

   for (i = 1; i < argc; i++)
   {
      if (!strcmp(argv[i], "--rom") && i + 1 < argc)
         rom_path = argv[++i];
      else if (!strcmp(argv[i], "--bios-dir") && i + 1 < argc)
         bios_dir = argv[++i];
      else if (!strcmp(argv[i], "--save") && i + 1 < argc)
         save_path = argv[++i];
      else if (!strcmp(argv[i], "--log") && i + 1 < argc)
         log_path = argv[++i];
      else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc)
         dump_dir = argv[++i];
      else if (!strcmp(argv[i], "--autoexit") && i + 1 < argc)
         autoexit = strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--dump-at") && i + 1 < argc)
         dump_at = strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc)
         dump_every = strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
         scale = (int)strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--no-pacing"))
         no_pacing = 1;
      else if (!strcmp(argv[i], "--ff"))
         force_ff = 1;
      else if (!strcmp(argv[i], "--script") && i + 1 < argc)
         script = argv[++i];
      else if (!strcmp(argv[i], "--force-sram-write"))
         force_sram_write = 1;
      else if (!strcmp(argv[i], "--host"))
      {
         net_host = 1;
         if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            net_port = strtol(argv[++i], NULL, 0);
      }
      else if (!strcmp(argv[i], "--join") && i + 1 < argc)
         net_join = argv[++i];
      else if (!strcmp(argv[i], "--nick") && i + 1 < argc)
         nick = argv[++i];
      else if (!strcmp(argv[i], "--net-loss") && i + 1 < argc)
         net_loss = (int)strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--net-jitter") && i + 1 < argc)
         net_jitter = (int)strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--net-probe"))
         net_probe = 1;
      else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
      {
         if (trace_add(argv[++i]) != 0)
         {
            fprintf(stderr, "bad --trace spec: %s\n", argv[i]);
            return 2;
         }
      }
      else if (!strcmp(argv[i], "--rfu-pace") && i + 1 < argc)
         rfu_pace = (int)strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--rfu-cushion") && i + 1 < argc)
         rfu_cush = (int)strtol(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--me-capture-test"))
      {
         me_capture_buf  = g_mecap_test_buf;
         me_capture_mode = 2;    /* capture AND render: the validate mode */
      }
      else if (!strcmp(argv[i], "--option") && i + 1 < argc)
      {
         char *kv = argv[++i];
         char *eq = strchr(kv, '=');
         if (eq)
         {
            *eq = '\0';
            if (fe_host_option_set(kv, eq + 1) != 0)
               fprintf(stderr, "unknown core option: %s\n", kv);
         }
      }
      else if (argv[i][0] != '-' && !rom_path)
         rom_path = argv[i];
      else
      {
         fprintf(stderr, "unknown arg: %s\n", argv[i]);
         return 2;
      }
   }

   if (!rom_path)
   {
      fprintf(stderr, "usage: gpsp_sdl --rom FILE [--bios-dir DIR] [--save FILE]"
                      " [--log FILE] [--autoexit N] [--dump-at N]\n");
      return 2;
   }

   if (!save_path)
   {
      /* default: <rom minus extension>.sav */
      size_t n = strlen(rom_path);
      const char *dot = strrchr(rom_path, '.');
      if (dot)
         n = (size_t)(dot - rom_path);
      if (n > sizeof(save_buf) - 5)
         n = sizeof(save_buf) - 5;
      memcpy(save_buf, rom_path, n);
      strcpy(save_buf + n, ".sav");
      save_path = save_buf;
   }

   fe_evt_init(log_path, 1);

   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
   {
      /* Video may be unavailable in CI; retry audio-only, then bare. */
      if (SDL_Init(SDL_INIT_AUDIO) != 0)
         SDL_Init(0);
   }

   if (SDL_WasInit(SDL_INIT_VIDEO))
   {
      window = SDL_CreateWindow("gpsp-adhoc (desktop twin)",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                FE_GBA_WIDTH * scale, FE_GBA_HEIGHT * scale, 0);
      if (window)
         renderer = SDL_CreateRenderer(window, -1, 0);
      if (renderer)
         texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     FE_GBA_WIDTH, FE_GBA_HEIGHT);
      have_video = (texture != NULL);
   }

   if (SDL_WasInit(SDL_INIT_AUDIO))
   {
      SDL_AudioSpec want, got;
      memset(&want, 0, sizeof(want));
      want.freq     = 65536;
      want.format   = AUDIO_S16SYS;
      want.channels = 2;
      want.samples  = 1024;
      audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
      if (audio_dev)
         SDL_PauseAudioDevice(audio_dev, 0);
   }

   fe_evt("boot_ok");
   fe_evt("clock=host");
   fe_evt("ff mode=%s", force_ff ? "uncapped" : "normal");

   memset(&cfg, 0, sizeof(cfg));
   cfg.rom_path      = rom_path;
   cfg.system_dir    = bios_dir;
   cfg.save_path     = save_path;
   cfg.video_frame   = plat_video_frame;
   cfg.audio_frames  = plat_audio_frames;
   cfg.input_bitmask = plat_input_bitmask;
   cfg.time_us       = net_now_us;   /* heartbeat t_us (hw perf baseline) */

   if (fe_host_boot(&cfg) != 0)
   {
      fe_evt("exit code=1 reason=load_failed");
      return 1;
   }

   if (rfu_pace >= 0)
   {
      rfu_set_frame_pace((unsigned)rfu_pace);
      fe_evt("rfu_frame_pace n=%d", rfu_pace);
   }
   if (rfu_cush >= 0)
   {
      rfu_set_cushion((unsigned)rfu_cush);
      fe_evt("rfu_cushion n=%d", rfu_cush);
   }

   if (script && fe_autopilot_load(script) != 0)
   {
      fe_evt("exit code=2 reason=bad_script");
      fe_host_shutdown();
      return 2;
   }

   if (net_host || net_join)
   {
      fe_np_config npc;
      nd_transport tp;
      char join_ip[64] = { 0 };
      uint16_t join_port = NET_DEFAULT_PORT;

      if (net_join)
      {
         const char *colon = strrchr(net_join, ':');
         size_t iplen = colon ? (size_t)(colon - net_join) : strlen(net_join);
         if (iplen >= sizeof(join_ip))
            iplen = sizeof(join_ip) - 1;
         memcpy(join_ip, net_join, iplen);
         if (colon)
            join_port = (uint16_t)strtol(colon + 1, NULL, 0);
         udp = udp_transport_create(0, join_ip, join_port);
      }
      else
         udp = udp_transport_create((uint16_t)net_port, NULL, 0);

      if (!udp)
      {
         fe_evt("exit code=4 reason=net_bind_failed");
         fe_host_shutdown();
         return 4;
      }
      if (net_loss || net_jitter)
      {
         udp_transport_set_fault(udp, net_loss, 0, 0, net_jitter,
                                 (uint32_t)net_now_us());
         fe_evt("net_fault loss=%d jitter=%d", net_loss, net_jitter);
      }

      memset(&npc, 0, sizeof(npc));
      udp_transport_iface(udp, &tp);
      npc.transport = &tp;
      npc.is_host   = net_host;
      npc.nick      = nick;
      npc.now_us    = net_now_us;
      npc.probe     = net_probe;
      if (fe_np_start(&npc) != 0)
      {
         fe_evt("exit code=4 reason=net_start_failed");
         udp_transport_destroy(udp);
         fe_host_shutdown();
         return 4;
      }
   }

   {
      const double frame_s = 1.0 / FE_GBA_FPS;
      const Uint64 pfreq = SDL_GetPerformanceFrequency();
      double next_deadline = (double)SDL_GetPerformanceCounter() / pfreq + frame_s;

      while (running)
      {
         SDL_Event ev;
         while (SDL_PollEvent(&ev))
         {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
               running = 0;
         }

         g_ff = force_ff || fe_autopilot_ff();
         if (fe_np_session_active())
            g_ff = 0;   /* FF interlock during wireless session (plan §4.4) */
         {
            /* ADR-0021 shape: time the work half of the iteration (pacing
             * sleep excluded) so fe_np_prof.frame_us means the same thing
             * here as it does on the PSP. */
            uint64_t w0 = net_now_us();
            fe_np_pump();  /* deliver inbound packets between retro_run calls */
            fe_autopilot_frame();
            fe_host_run_frame();
            fe_np_prof_frame((uint32_t)(net_now_us() - w0));
         }
         rfu_trace_drain();
         sess_poll_evt();

         if ((dump_at && fe_host_frame_count() == (unsigned)dump_at) ||
             (dump_every && (fe_host_frame_count() % (unsigned)dump_every) == 0) ||
             fe_autopilot_dump_pending())
         {
            size_t pitch;
            const uint16_t *pix = fe_host_last_frame(&pitch);
            if (pix)
            {
               char path[600];
               snprintf(path, sizeof(path), "%s/frame_%06u.bmp",
                        dump_dir, fe_host_frame_count());
               if (fe_bmp_write_rgb565(path, pix, FE_GBA_WIDTH, FE_GBA_HEIGHT,
                                       pitch) == 0)
                  fe_evt("frame_dump file=%s", path);
            }
         }

         if (script)
         {
            int st = fe_autopilot_status();
            if (st == 1)
               running = 0;              /* script finished: clean exit */
            else if (st == -1)
            {
               exit_code = 3;
               exit_reason = "ap_fail";
               running = 0;
            }
         }

         if (g_trace_n && (fe_host_frame_count() % 300) == 0)
            trace_emit();

         if (autoexit && fe_host_frame_count() >= (unsigned)autoexit)
            running = 0;

         if (!no_pacing && !g_ff)
         {
            double now = (double)SDL_GetPerformanceCounter() / pfreq;
            if (now < next_deadline)
               SDL_Delay((Uint32)((next_deadline - now) * 1000.0));
            next_deadline += frame_s;
            now = (double)SDL_GetPerformanceCounter() / pfreq;
            if (next_deadline < now - 0.25)  /* fell behind badly: resync */
               next_deadline = now + frame_s;
         }
      }
   }

   fe_np_stop();
   if (udp)
      udp_transport_destroy(udp);
   if (force_sram_write)
      fe_host_sram_flush(1);
   fe_host_shutdown();

   if (audio_dev)
      SDL_CloseAudioDevice(audio_dev);
   if (texture)
      SDL_DestroyTexture(texture);
   if (renderer)
      SDL_DestroyRenderer(renderer);
   if (window)
      SDL_DestroyWindow(window);
   SDL_Quit();

   if (exit_reason)
      fe_evt("exit code=%d reason=%s", exit_code, exit_reason);
   else
      fe_evt("exit code=%d", exit_code);
   fe_evt_close();
   return exit_code;
}
