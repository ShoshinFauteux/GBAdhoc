#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

/* SHA-1 of the official GBA BIOS (Nintendo AGB BIOS, 16 KiB). */
#define GBA_BIOS_SHA1 "300c20df6731a33952ded8c436f7f186d25d3492"
#define SRAM_CHECK_INTERVAL 300   /* frames ~= 5 s (FRONTEND-AUDIT §5) */
#define HEARTBEAT_INTERVAL  600

/* ---------------------------------------------------------------- state -- */

static fe_host_config host;

/* Core options (FRONTEND-AUDIT §9). Values must match check_variables()
 * string compares exactly (libretro/libretro.c:918-1120). */
typedef struct { const char *key; char value[24]; } fe_option;
static fe_option options[] = {
   { "gpsp_bios",                 "official" },   /* plan §3.4: real BIOS */
   { "gpsp_boot_mode",            "game"     },
   { "gpsp_drc",                  "enabled"  },
   { "gpsp_rtc",                  "auto"     },
   { "gpsp_serial",               "auto"     },   /* auto => RFU for BPEE/BPRE/BPGE */
   { "gpsp_rumble",               "disabled" },   /* no rumble interface provided */
   /* "No Sprite Limit" — `disabled` KEEPS the GBA's per-scanline sprite
    * limit, which is both hardware-accurate and the cheaper of the two.
    * Enabling it would render sprites real hardware drops. Do not "optimise"
    * this by turning it on; the name reads backwards. */
   { "gpsp_sprlim",               "disabled" },
   /* ADR-0028: 32768, not 65536. The core's own option text: "Both values
    * keep audio timing exact. 65536 renders the full mixer bandwidth; 32768
    * matches the bandwidth of real hardware's default PWM output and halves
    * audio mixing work." Real GBA PWM output is 32768 Hz, so 65536 buys
    * bandwidth the console never produced, at double the mixing cost —
    * inside retro_run, which is exactly where the field's 11-12 ms mean and
    * 21-29 ms max live. NOTE: platforms must take their resampler input rate
    * from fe_host_sample_rate(), never from a literal. */
   { "gpsp_sound_rate",           "32768"    },
   { "gpsp_frameskip",            "disabled" },
   { "gpsp_frameskip_threshold",  "33"       },
   { "gpsp_frameskip_interval",   "1"        },
   /* KEEP BOTH DISABLED ON PSP (ADR-0039).  Frame mixing is layout-agnostic
    * -- its 0x821 mask is the low bit of each 5/6/5 field either way -- but
    * colour correction indexes gba_cc_lut, a 32768-entry table BAKED IN
    * libretro RGB565 channel order (tools/generate_cc_lut.c).  Under
    * USE_PSP_RGB565_FORMAT the index would hand red's curve to blue and
    * hand back a libretro-ordered pixel, which is a colour bug that looks
    * like a GE bug.  fe_host_option_set_live() refuses to turn it on rather
    * than trusting this comment. */
   { "gpsp_color_correction",     "disabled" },
   { "gpsp_frame_mixing",         "disabled" },
   { "gpsp_turbo_period",         "4"        },
};

static unsigned frame_count;
/* Core's audio output rate, learned from retro_get_system_av_info at boot
 * (ADR-0028). 0 until fe_host_boot succeeds. */
static unsigned core_sample_rate;
static const uint16_t *last_frame;
static size_t last_pitch;

/* Rendered-vs-emulated accounting (ADR-0019).  The core signals a skipped
 * VIDEO frame by calling video_refresh with data==NULL, so these two
 * counters are the only honest way to tell "the console is behind" from
 * "the console is fine but we threw pictures away".  The field's first
 * session looked healthy on `EVT heartbeat` alone (58.9 fps sustained)
 * while frameskip was quietly halving what the user actually saw. */
static unsigned frames_rendered;
static unsigned frames_skipped;
static unsigned fps_last_frames, fps_last_rendered, fps_last_skipped;
static uint64_t fps_last_us;

/* Set when an option changed after boot; the core re-reads its options on
 * the next retro_run via GET_VARIABLE_UPDATE (runtime-changeable keys
 * only, e.g. gpsp_frameskip for the FF feature — plan §4.4). */
static int options_dirty;

static uint32_t sram_last_crc;
static int      sram_have_crc;

/* Per-block SRAM dirty map (ADR-0020).  4 KiB matches the GBA flash sector
 * the games erase/write one at a time, so a mid-save dirty check normally
 * touches 1-2 blocks instead of rewriting all 128 KiB to the memory stick. */
#define SRAM_BLOCK   4096
#define SRAM_BLOCKS  (FE_SRAM_SIZE / SRAM_BLOCK)
static uint32_t sram_blk_crc[SRAM_BLOCKS];
static int      sram_have_blk;   /* the .sav on disk matches sram_blk_crc */

/* Netpacket interface copy — unused in Phase 1, wired in Phase 3/4. */
static struct retro_netpacket_callback netpacket_cb;
static int netpacket_registered;

/* Audio-buffer status sink the core installs for gpsp_frameskip=auto
 * (ADR-0018). NULL until the core asks for it / after it disables it. */
static retro_audio_buffer_status_callback_t audio_buf_status_cb;

/* Memory map copy (autopilot RAM predicates, plan §7.1). */
#define FE_MAX_MEMDESC 8
static struct retro_memory_descriptor memdesc[FE_MAX_MEMDESC];
static unsigned memdesc_count;

/* ------------------------------------------------------------ callbacks -- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   /* strip trailing newline; fe_log adds one */
   {
      size_t n = strlen(buf);
      while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
         buf[--n] = '\0';
   }
   fe_log("core[%d]: %s", (int)level, buf);
}

static void video_refresh(const void *data, unsigned width, unsigned height,
                          size_t pitch)
{
   if (data)
   {
      last_frame = (const uint16_t *)data;
      last_pitch = pitch;
      frames_rendered++;
   }
   else
      frames_skipped++;          /* core frameskip: nothing new to present */
   if (host.video_frame)
      host.video_frame((const uint16_t *)data, width, height, pitch);
}

static void audio_sample(int16_t left, int16_t right)
{
   (void)left; (void)right; /* core's per-sample setter is a stub; batch only */
}

static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
   if (host.audio_frames)
      host.audio_frames(data, frames);
   return frames;
}

static void input_poll(void)
{
   /* Platform samples inside input_bitmask(); nothing to do here. */
}

static uint32_t injected_mask;   /* autopilot input (fe_host_input_inject) */

static int16_t input_state(unsigned port, unsigned device, unsigned index,
                           unsigned id)
{
   uint32_t mask;
   if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0)
      return 0;
   mask = (host.input_bitmask ? host.input_bitmask() : 0) | injected_mask;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)mask;
   return (mask >> id) & 1;
}

static bool env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
   {
      struct retro_log_callback *cb = (struct retro_log_callback *)data;
      cb->log = core_log;
      return true;
   }

   case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned *)data = 1;
      return true;

   /* Option registration — we keep our own fixed table; ack and ignore. */
   case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
   case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
   case RETRO_ENVIRONMENT_SET_VARIABLES:
      return true;

   case RETRO_ENVIRONMENT_GET_VARIABLE:
   {
      struct retro_variable *var = (struct retro_variable *)data;
      size_t i;
      var->value = NULL;
      for (i = 0; i < sizeof(options) / sizeof(options[0]); i++)
      {
         if (strcmp(options[i].key, var->key) == 0)
         {
            var->value = options[i].value;
            return true;
         }
      }
      return false;
   }

   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = options_dirty ? true : false;
      options_dirty = 0;
      return true;

   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
   {
      enum retro_pixel_format fmt = *(const enum retro_pixel_format *)data;
      if (fmt != RETRO_PIXEL_FORMAT_RGB565)
      {
         fe_log("core requested unsupported pixel format %d", (int)fmt);
         return false;
      }
      return true;
   }

   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *(const char **)data = host.system_dir;
      return host.system_dir != NULL;

   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;

   case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return true;

   case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
   {
      const struct retro_memory_map *mm = (const struct retro_memory_map *)data;
      unsigned n = mm->num_descriptors;
      if (n > FE_MAX_MEMDESC)
         n = FE_MAX_MEMDESC;
      memcpy(memdesc, mm->descriptors, n * sizeof(memdesc[0]));
      memdesc_count = n;
      return true;
   }

   case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
   {
      /* The multiplayer seam (FRONTEND-AUDIT §1.2 #9). Copy and hold;
       * netpacket_host (Phase 3) fetches it via fe_host_netpacket_cb()
       * and calls start/stop on netdrv session events. */
      memcpy(&netpacket_cb, data, sizeof(netpacket_cb));
      netpacket_registered = 1;
      fe_log("netpacket interface registered (proto=\"%s\")",
             netpacket_cb.protocol_version ? netpacket_cb.protocol_version
                                           : "(null)");
      return true;
   }

   case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 0;  /* core then uses legacy SET_MESSAGE */
      return true;

   case RETRO_ENVIRONMENT_SET_MESSAGE:
   {
      const struct retro_message *msg = (const struct retro_message *)data;
      fe_log("core-msg: %s", msg->msg);
      return true;
   }

   case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
      return true;   /* platforms ring-buffer generously anyway */

   case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      /* Only fired on mid-session gpsp_sound_rate change, which we never do
       * (options are fixed before boot). Refuse so the core keeps the old
       * rate rather than assuming we renegotiated. */
      return false;

   case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
   {
      /* Accepted since ADR-0018 so gpsp_frameskip=auto works: the core
       * skips a VIDEO frame when the frontend reports an imminent audio
       * underrun, i.e. exactly when the platform is failing to hold real
       * time. That keeps game logic — and the ARQ pump riding on it — at
       * wall-clock speed on slower consoles during a wireless session.
       * data==NULL means the core is disabling the callback. */
      const struct retro_audio_buffer_status_callback *cb =
         (const struct retro_audio_buffer_status_callback *)data;
      audio_buf_status_cb = cb ? cb->callback : NULL;
      return true;
   }

   /* Explicitly declined (safe-false per FRONTEND-AUDIT §1):
    * perf, VFS (built-in PSP path preferred), language, rumble,
    * fast-forward override (frontend owns FF, plan §4.4). */
   case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
   case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
   case RETRO_ENVIRONMENT_GET_LANGUAGE:
   case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
   case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
      return false;

   default:
      fe_log("env: unhandled cmd %u -> false", cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL);
      return false;
   }
}

/* ----------------------------------------------------------------- sram -- */

static uint32_t sram_crc(void)
{
   const void *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   return p ? fe_crc32(0, p, FE_SRAM_SIZE) : 0;
}

static void sram_load(void)
{
   void *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   FILE *f;
   size_t n = 0;
   int exact = 0;

   if (!p || !host.save_path)
      return;

   f = fopen(host.save_path, "rb");
   if (f)
   {
      n = fread(p, 1, FE_SRAM_SIZE, f);
      exact = (n == FE_SRAM_SIZE) && (fgetc(f) == EOF);   /* no trailing junk */
      fclose(f);
      fe_evt("sram_load size=%u crc=%08x", (unsigned)n, (unsigned)sram_crc());
   }
   else
      fe_evt("sram_load size=0 crc=none");

   sram_last_crc = sram_crc();
   sram_have_crc = 1;

   /* Seed the per-block dirty map (ADR-0020) only when the file on disk is
    * exactly a 128 KiB image of what we just loaded — otherwise the first
    * flush must rewrite (and truncate) the whole thing before deltas mean
    * anything. */
   sram_have_blk = 0;
   if (exact)
   {
      unsigned i;
      for (i = 0; i < SRAM_BLOCKS; i++)
         sram_blk_crc[i] = fe_crc32(0, (const uint8_t *)p + (size_t)i *
                                    SRAM_BLOCK, SRAM_BLOCK);
      sram_have_blk = 1;
   }
}

uint32_t fe_host_sram_crc_now(void)
{
   return sram_crc();
}

/* ADR-0046: exactly what input_state() hands the core, pad OR injected.
 *
 * The first version of the input probe read the platform pad directly, which
 * meant the harness -- which drives everything through fe_host_input_inject --
 * could never exercise it.  The gate then "passed" on a single priming line.
 * Reporting the value the core actually consumes is both the truthful number
 * and the one the rig can test. */
uint32_t fe_host_input_mask(void)
{
   return (host.input_bitmask ? host.input_bitmask() : 0) | injected_mask;
}

void fe_host_input_inject(uint32_t joypad_mask)
{
   injected_mask = joypad_mask;
}

int fe_host_mem_read(uint32_t gba_addr, void *out, unsigned len)
{
   unsigned i;

   for (i = 0; i < memdesc_count; i++)
   {
      const struct retro_memory_descriptor *d = &memdesc[i];
      if (!d->ptr || !d->len)
         continue;
      if (gba_addr >= d->start && gba_addr + len <= d->start + d->len)
      {
         memcpy(out, (const uint8_t *)d->ptr + d->offset +
                        (gba_addr - d->start), len);
         return 0;
      }
   }

   /* Fallback: EWRAM through the plain memory-data API. */
   if (gba_addr >= 0x02000000 && gba_addr + len <= 0x02040000)
   {
      const uint8_t *p =
         (const uint8_t *)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
      if (p)
      {
         memcpy(out, p + (gba_addr - 0x02000000), len);
         return 0;
      }
   }
   return -1;
}

/* ADR-0079: mirror of the read.  RAM regions only — a descriptor that maps
 * ROM is skipped, because writing gamepak space through a stale descriptor
 * is how an emulator corrupts a translation cache. */
int fe_host_mem_write(uint32_t gba_addr, const void *in, unsigned len)
{
   unsigned i;

   if (gba_addr >= 0x08000000)
      return -1;
   for (i = 0; i < memdesc_count; i++)
   {
      const struct retro_memory_descriptor *d = &memdesc[i];
      if (!d->ptr || !d->len)
         continue;
      if (gba_addr >= d->start && gba_addr + len <= d->start + d->len)
      {
         memcpy((uint8_t *)d->ptr + d->offset + (gba_addr - d->start), in,
                len);
         return 0;
      }
   }
   if (gba_addr >= 0x02000000 && gba_addr + len <= 0x02040000)
   {
      uint8_t *p = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
      if (p)
      {
         memcpy(p + (gba_addr - 0x02000000), in, len);
         return 0;
      }
   }
   return -1;
}

/* ------------------------------------------------------------ savestate -- */

/* retro_serialize_size is the constant GBA_STATE_MEM_SIZE = 416 KiB
 * (FRONTEND-AUDIT §7) and both serialize calls REQUIRE exactly that size.
 * Static staging buffer: .bss, never fights the core's greedy ROM heap. */
#define FE_STATE_MAX (416 * 1024)
static uint8_t state_buf[FE_STATE_MAX];

int fe_host_state_save(const char *path)
{
   size_t sz = retro_serialize_size();
   FILE *f;

   if (sz == 0 || sz > sizeof(state_buf) || !retro_serialize(state_buf, sz))
   {
      fe_evt("state_save file=%s FAILED reason=serialize sz=%u", path,
             (unsigned)sz);
      return -1;
   }
   f = fopen(path, "wb");
   if (!f || fwrite(state_buf, 1, sz, f) != sz)
   {
      if (f)
         fclose(f);
      fe_evt("state_save file=%s FAILED reason=io", path);
      return -1;
   }
   fclose(f);
   fe_evt("state_save file=%s size=%u crc=%08x", path, (unsigned)sz,
          (unsigned)fe_crc32(0, state_buf, sz));
   return 0;
}

int fe_host_state_load(const char *path)
{
   size_t sz = retro_serialize_size();
   size_t n;
   FILE *f = fopen(path, "rb");

   if (!f)
   {
      fe_evt("state_load file=%s FAILED reason=open", path);
      return -1;
   }
   n = fread(state_buf, 1, sizeof(state_buf), f);
   fclose(f);
   if (n != sz || !retro_unserialize(state_buf, sz))
   {
      fe_evt("state_load file=%s FAILED reason=unserialize n=%u", path,
             (unsigned)n);
      return -1;
   }
   fe_evt("state_load file=%s size=%u", path, (unsigned)sz);
   return 0;
}

/* ---- SRAM persistence (ADR-0020) ----------------------------------------
 *
 * The old path rewrote all 131,072 bytes to the memory stick synchronously
 * on the emulation thread whenever the whole-image CRC moved.  Two field
 * logs put that at ~0.5-1.3 s of FROZEN frame loop per flush (heartbeat
 * windows containing `EVT sram_flush` ran 50.4-50.7 fps on the PSP-1000
 * against 56.5-57.6 fps either side).  That is not just lag: the
 * RetroArch x2 bisection walked out of the Union Room cleanly on a PC,
 * where writing the save is free, while both PSPs showed the game's FATAL
 * wireless screen at exactly the moment leaving the room makes the game
 * save.  A second of not servicing the link mid-teardown-handshake is
 * indistinguishable from a dead peer.
 *
 * So the requirement is not "write less", it is **never stall the emulation
 * thread for long, at any point, on any hardware**.  Three things together:
 *
 *   1. Dirty 4 KiB blocks only.  4 KiB is the GBA flash sector the games
 *      erase and write one at a time, so a mid-save check finds one or two.
 *   2. The file is held OPEN for the run ("r+b").  Re-opening per flush
 *      pays FAT/directory cost that would otherwise dominate once writes
 *      are small, and it is per-frame cost under (3).
 *   3. Writes are drained under a WALL-CLOCK BUDGET, a few blocks per
 *      frame, continuing on following frames until the dirty set is empty.
 *      One block may overshoot; nothing else can.  This is self-limiting on
 *      hardware whose write throughput we do not know and cannot measure
 *      from the PPSSPP rig (its ms0 is a host filesystem).
 *
 * Save safety is not traded away: the same dirty check, the same flush
 * points, and exit/menu flushes still drain to completion before returning.
 * It is strictly safer than before on power loss, because "wb" truncated
 * the file before rewriting it and an in-place block update never does.
 */
#define SRAM_FLUSH_BUDGET_US 3000   /* per-frame slice (~18 % of a frame) */

/* ---- ADR-0025: the writes leave the emulation thread ---------------------
 * ADR-0020 bounded the stall to one 3 ms budget plus one block's overshoot.
 * The field says that is not enough: `EVT sram_flush ... wrote=57344
 * blocks=14/32 mode=delta ms=29.296`, in the same heartbeat window where
 * `frame` max went 37264 -> 63772 us.  A 4 KiB write to a memory stick is
 * not a bounded operation on this hardware, so it must not be on the frame
 * path at all.
 *
 * The dirty map is now a per-block BYTE state, not a bit-set, because two
 * threads share it and a byte store is atomic on MIPS where a read-modify-
 * write on a bitmap word is not:
 *   CLEAN   -> on disk, and sram_blk_crc[i] proves it
 *   DIRTY   -> the emulation thread's scan wants it written
 *   WRITING -> the writer thread has taken it
 * The emulation thread only ever raises DIRTY and never clears; the writer
 * only ever CLEAN-s a block it still owns (`state == WRITING` after the
 * write).  So a block the game touches mid-write is re-marked DIRTY, the
 * writer's compare-and-clear fails, and the next pass rewrites it.  That is
 * the whole synchronisation, and it needs no lock. */
#define SBLK_CLEAN    0
#define SBLK_DIRTY    1
#define SBLK_WRITING  2

static FILE    *sram_fp;            /* held open "r+b"; WRITER-owned */
static volatile unsigned char sram_blk_state[SRAM_BLOCKS];
static volatile int sram_full_req;  /* whole-image rewrite pending */
/* 0 = none, 1 = periodic scan due, 2 = forced (write even if clean). */
static volatile int sram_scan_req;
static uint64_t sram_last_scan_us;  /* writer-owned scan cadence */

static const fe_host_io *host_io;   /* NULL = ADR-0020 synchronous path */

/* Bounded wait for a forced flush: 4000 x ~1 ms.  A memory stick that has
 * not absorbed 128 KiB in four seconds is broken, and we say so in the log
 * rather than spinning forever inside an exit path. */
#define SRAM_SYNC_MAX_SPINS 4000

/* Writer-thread scan cadence.  Deliberately the same ~5 s as the emulation
 * thread's SRAM_CHECK_INTERVAL (300 frames) so the window in which a save
 * can be lost to a power cut is unchanged by ADR-0026 — this moves the
 * work, it does not relax the guarantee. */
#define SRAM_SCAN_INTERVAL_US 5000000ull

/* Accumulators for the one EVT emitted when a drain completes. */
static uint32_t sram_run_bytes;
static unsigned sram_run_blocks;
static uint64_t sram_run_us;        /* total time inside the writer */
static uint64_t sram_run_slice_max; /* worst single drain call */
static uint64_t sram_run_blk_max;   /* worst single 4 KiB block write */
static unsigned sram_run_slices;    /* drain calls this write took */
static uint32_t sram_run_crc;
static int      sram_run_full;
static int      sram_run_threaded;
/* The per-block CRC scan stays on the EMULATION thread — it is CPU, not
 * I/O, and moving it would mean snapshotting 128 KiB.  It is reported so
 * that "the save path costs the emulator nothing" is a measurement rather
 * than an assumption: after ADR-0025 this is the only part of a save the
 * emulation thread still pays for. */
static uint64_t sram_run_scan_us;

/* Staging copy of the block actually written, so the CRC we record is the
 * CRC of the bytes on disk even if the game rewrites the block underneath
 * us.  Only ever touched by whichever single thread is draining. */
static uint8_t  sram_stage[SRAM_BLOCK];

/* Defined below with the flush path; the writer thread calls it too. */
static int sram_scan(int force_write);

void fe_host_set_io(const fe_host_io *io)
{
   host_io = io;
   if (io)
   {
      /* Start the writer's scan clock now, so the first sweep lands one
       * interval from here rather than immediately at session bring-up. */
      uint64_t now = host.time_us ? host.time_us() : 0;
      sram_last_scan_us = now ? now : 1;
   }
}

static unsigned sram_pending(void)
{
   unsigned i, n = 0;
   for (i = 0; i < SRAM_BLOCKS; i++)
      if (sram_blk_state[i] != SBLK_CLEAN)
         n++;
   return n;
}

static void sram_close(void)
{
   if (sram_fp)
   {
      fclose(sram_fp);
      sram_fp = NULL;
   }
}

/* Give up on incremental updates: the next scan rewrites the whole image. */
static void sram_abandon_deltas(const char *why)
{
   unsigned i;
   fe_log("sram_flush: %s — falling back to a full rewrite", why);
   sram_close();
   sram_have_blk = 0;
   for (i = 0; i < SRAM_BLOCKS; i++)
      sram_blk_state[i] = SBLK_CLEAN;
   sram_full_req = 0;
}

/* Rewrite the whole .sav.  First flush of a run, or any time an in-place
 * update is impossible (file missing / wrong size / seek or write failed). */
static int sram_write_full(const void *p)
{
   FILE *f;
   sram_close();
   f = fopen(host.save_path, "wb");
   if (!f)
   {
      fe_log("sram_flush FAILED to open %s", host.save_path);
      return -1;
   }
   if (fwrite(p, 1, FE_SRAM_SIZE, f) != FE_SRAM_SIZE)
   {
      fclose(f);
      fe_log("sram_flush short write to %s", host.save_path);
      return -1;
   }
   fclose(f);
   return 0;
}

/* Write pending dirty blocks until the dirty set is empty or `budget_us` of
 * wall clock has been spent (0 = drain to completion — exit/menu paths).
 * Always writes at least one block when work is pending, so progress is
 * guaranteed even with a zero-length budget.  Returns -1 on I/O error. */
static int sram_drain(uint32_t budget_us)
{
   const uint8_t *p = (const uint8_t *)
      retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   uint64_t t0, slice;
   unsigned i;

   if (!p || !host.save_path)
      return 0;

   /* A full rewrite is requested when there is no trustworthy on-disk image
    * to patch (first flush of a run, or after an I/O failure).  It happens
    * at most once per run and normally before any session is up — but it is
    * still 128 KiB, so it too runs here, on whichever thread is draining. */
   if (sram_full_req)
   {
      t0 = host.time_us ? host.time_us() : 0;
      if (sram_write_full(p) != 0)
      {
         sram_full_req = 0;
         return -1;
      }
      slice = (host.time_us ? host.time_us() : 0) - t0;
      sram_run_us += slice;
      sram_run_slices++;
      if (slice > sram_run_slice_max)
         sram_run_slice_max = slice;
      if (slice > sram_run_blk_max)
         sram_run_blk_max = slice;
      for (i = 0; i < SRAM_BLOCKS; i++)
      {
         sram_blk_crc[i] = fe_crc32(0, p + (size_t)i * SRAM_BLOCK,
                                    SRAM_BLOCK);
         sram_blk_state[i] = SBLK_CLEAN;
      }
      sram_run_bytes += FE_SRAM_SIZE;
      sram_run_blocks = SRAM_BLOCKS;
      sram_run_full = 1;
      sram_have_blk = 1;
      sram_full_req = 0;
   }
   else if (!sram_pending())
      return 0;

   if (sram_pending())
   {
      if (!sram_fp)
      {
         sram_fp = fopen(host.save_path, "r+b");
         if (!sram_fp)
         {
            sram_abandon_deltas("cannot reopen the .sav for update");
            return -1;
         }
      }

      t0 = host.time_us ? host.time_us() : 0;
      for (i = 0; i < SRAM_BLOCKS; i++)
      {
         uint64_t b0, bd;
         if (sram_blk_state[i] != SBLK_DIRTY)
            continue;
         /* Take the block, then copy it.  Anything the game writes from
          * here on re-raises DIRTY and the compare-and-clear below fails,
          * so the next pass comes back for it. */
         sram_blk_state[i] = SBLK_WRITING;
         memcpy(sram_stage, p + (size_t)i * SRAM_BLOCK, SRAM_BLOCK);

         b0 = host.time_us ? host.time_us() : 0;
         if (fseek(sram_fp, (long)i * SRAM_BLOCK, SEEK_SET) != 0 ||
             fwrite(sram_stage, 1, SRAM_BLOCK, sram_fp) != SRAM_BLOCK ||
             fflush(sram_fp) != 0)
         {
            sram_abandon_deltas("seek/write/flush failed mid-update");
            return -1;
         }
         bd = (host.time_us ? host.time_us() : 0) - b0;
         if (bd > sram_run_blk_max)
            sram_run_blk_max = bd;

         /* CRC the STAGED bytes — i.e. exactly what is now on disk. */
         sram_blk_crc[i] = fe_crc32(0, sram_stage, SRAM_BLOCK);
         if (sram_blk_state[i] == SBLK_WRITING)
            sram_blk_state[i] = SBLK_CLEAN;   /* still ours: it is clean */
         sram_run_bytes += SRAM_BLOCK;
         sram_run_blocks++;

         if (budget_us && host.time_us &&
             host.time_us() - t0 >= (uint64_t)budget_us)
            break;                  /* out of slice — resume next frame */
      }
      slice = (host.time_us ? host.time_us() : 0) - t0;
      sram_run_us += slice;
      sram_run_slices++;
      if (slice > sram_run_slice_max)
         sram_run_slice_max = slice;
   }

   if (!sram_pending())
   {
      /* `ms=` was ambiguous in the field: it is the TOTAL time spent
       * writing, which under ADR-0020's budget is spread across however
       * many frames it took.  `slices=` disambiguates it once and for all
       * (1 = ms is a single stall, >1 = elapsed across that many calls),
       * `worst_ms=` is the largest single call — the actual stall — and
       * `blk_ms=` is the worst single 4 KiB write, which is the thing the
       * budget cannot bound.  `thr=1` means none of it was on the
       * emulation thread.
       *
       * READ THESE AS WALL CLOCK, NOT CPU, WHEN thr=1 (ADR-0026): the
       * writer runs below main and is preempted constantly, so `ms` and
       * `scan_ms` measure how long it took to get through the work, not
       * what the work cost.  Under uncapped fast-forward — where main
       * never blocks at the vblank — the rig shows `scan_ms=2405`, and
       * that is the thread being starved, not 2.4 s of CRC32.  The number
       * that matters once thr=1 is `thr` itself. */
      fe_evt("sram_flush crc=%08x size=%u wrote=%u blocks=%u/%u mode=%s "
             "ms=%u.%03u slices=%u worst_ms=%u.%03u blk_ms=%u.%03u "
             "scan_ms=%u.%03u thr=%d",
             (unsigned)sram_run_crc, (unsigned)FE_SRAM_SIZE,
             (unsigned)sram_run_bytes, sram_run_blocks,
             (unsigned)SRAM_BLOCKS, sram_run_full ? "full" : "delta",
             (unsigned)(sram_run_us / 1000), (unsigned)(sram_run_us % 1000),
             sram_run_slices,
             (unsigned)(sram_run_slice_max / 1000),
             (unsigned)(sram_run_slice_max % 1000),
             (unsigned)(sram_run_blk_max / 1000),
             (unsigned)(sram_run_blk_max % 1000),
             (unsigned)(sram_run_scan_us / 1000),
             (unsigned)(sram_run_scan_us % 1000),
             sram_run_threaded);
      sram_run_bytes = 0;
      sram_run_blocks = 0;
      sram_run_us = 0;
      sram_run_slice_max = 0;
      sram_run_blk_max = 0;
      sram_run_slices = 0;
      sram_run_scan_us = 0;
      sram_run_full = 0;
   }
   return 0;
}

/* The writer thread's entry point.  No budget: nothing here is on a frame
 * path, so it drains everything it finds in one pass. */
int fe_host_sram_service_io(void)
{
   uint64_t now = host.time_us ? host.time_us() : 0;
   unsigned before;
   int req = sram_scan_req;

   /* The scan is on this thread too since ADR-0026: it is 11 ms of CPU on
    * the user's hardware, and on a single-core console the only place to
    * put 11 ms that does not cost the emulator a frame is the slack it
    * already donates at the vblank wait. */
   if (req || (sram_last_scan_us &&
               now - sram_last_scan_us >= SRAM_SCAN_INTERVAL_US))
   {
      sram_last_scan_us = now;
      sram_run_threaded = 1;
      (void)sram_scan(req == 2);
      sram_scan_req = 0;      /* clear only after the scan has marked */
   }

   if (!sram_full_req && !sram_pending())
      return 0;
   before = sram_pending();
   sram_run_threaded = 1;
   (void)sram_drain(0);
   { unsigned after = sram_pending();
     return (int)(before > after ? before - after : 0); }
}

/* Per-frame service: keep an in-progress flush moving without ever handing
 * the memory stick a whole 128 KiB image in one frame.  Called from
 * fe_host_run_frame; cheap no-op when nothing is pending. */
void fe_host_sram_service(void)
{
   if (host_io)
      return;              /* the writer thread owns the file (ADR-0025) */
   if (sram_pending() || sram_full_req)
      sram_drain(SRAM_FLUSH_BUDGET_US);
}

/* Finish any in-flight .sav write, however long it takes.  Every point that
 * must not lose bytes calls this: exit, and any future suspend/savestate
 * path.  Budgeted draining is a smoothness optimisation and must never
 * become a durability regression — without this, shutdown could close the
 * file with blocks still pending. */
void fe_host_sram_sync(void)
{
   if (!host_io)
   {
      if (sram_pending() || sram_full_req)
         sram_drain(0);
      return;
   }
   /* Async: the writer thread owns the file, so we must not touch it — we
    * wake it and YIELD until it is done.  Yielding is load-bearing, not
    * politeness: the writer deliberately runs below us, so a busy wait
    * would starve the very thread we are waiting for.
    *
    * If the bound expires we do NOT take the file over — that would put two
    * writers on one FILE*.  We say so in the log; the shutdown path stops
    * the writer thread and calls this again with host_io cleared, which
    * then drains synchronously and cannot fail to complete. */
   {
      int spins;
      host_io->wake();
      for (spins = 0; spins < SRAM_SYNC_MAX_SPINS; spins++)
      {
         /* A pending SCAN counts as outstanding work: a forced flush must
          * not return before the scan that decides what to write has even
          * run (ADR-0026). */
         if (!sram_scan_req && !sram_pending() && !sram_full_req)
            return;
         host_io->yield();
      }
   }
   fe_evt("sram_sync_timeout blocks=%u full=%d scan=%d", sram_pending(),
          sram_full_req, sram_scan_req);
}

/* The dirty scan, callable from EITHER thread (ADR-0026).
 *
 * It is pure CPU over the live 128 KiB buffer — no file access — but the
 * field priced it at 11.243 / 11.272 / 11.280 ms, i.e. two thirds of a
 * frame, paid every 300 frames WHETHER OR NOT anything was dirty.  That is
 * a guaranteed periodic stall, so it follows the writes onto the writer
 * thread rather than being spread across frames: spreading would still
 * spend the cycles on the emulation thread, and the whole point is that on
 * a single-core console those cycles have to come from somewhere the
 * emulator is not using.  The vblank slack is that somewhere.
 *
 * Racing the game is harmless and is the pre-existing behaviour: a block
 * read while the game writes it is simply seen as dirty, written, and
 * CRC'd from the staging copy — and if it moved again, the next sweep
 * catches it.  Returns 1 if anything needs writing. */
static int sram_scan(int force_write)
{
   const uint8_t *p = (const uint8_t *)
      retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   static uint32_t crc_now[SRAM_BLOCKS];
   uint32_t crc = 0;
   unsigned i;

   if (!p || !host.save_path)
      return 0;

   /* One pass: per-block CRC32 folded into the whole-image CRC.  fe_crc32
    * chains, so `crc` is bit-identical to the old single-shot call over the
    * whole image — every existing log line and oracle comparison matches,
    * and the `crc=` the EVT reports costs nothing extra. */
   {
      uint64_t s0 = host.time_us ? host.time_us() : 0;
      for (i = 0; i < SRAM_BLOCKS; i++)
      {
         crc_now[i] = fe_crc32(0, p + (size_t)i * SRAM_BLOCK, SRAM_BLOCK);
         crc = fe_crc32(crc, p + (size_t)i * SRAM_BLOCK, SRAM_BLOCK);
      }
      sram_run_scan_us += (host.time_us ? host.time_us() : 0) - s0;
   }
   if (!force_write && sram_have_crc && crc == sram_last_crc)
   {
      sram_run_scan_us = 0;    /* nothing to attribute it to */
      return 0;
   }

   sram_run_crc = crc;
   sram_run_threaded = host_io ? 1 : 0;

   if (!sram_have_blk)
   {
      /* No trustworthy on-disk image to patch: one full rewrite.  Requested
       * here, PERFORMED by whoever drains — so with a writer thread even
       * this 128 KiB pass is off the emulation thread. */
      sram_full_req = 1;
      for (i = 0; i < SRAM_BLOCKS; i++)
         sram_blk_state[i] = SBLK_CLEAN;
   }
   else
   {
      /* Only ever RAISE dirty here (ADR-0025): a block the writer thread is
       * mid-way through is left for its compare-and-clear to resolve, and a
       * block that is genuinely clean is already CLEAN. */
      for (i = 0; i < SRAM_BLOCKS; i++)
         if (crc_now[i] != sram_blk_crc[i])
            sram_blk_state[i] = SBLK_DIRTY;
      /* force_write means "write even when clean" (round-trip tests), and
       * that contract predates the dirty map — honour it literally. */
      if (force_write && !sram_pending())
         for (i = 0; i < SRAM_BLOCKS; i++)
            sram_blk_state[i] = SBLK_DIRTY;
   }

   sram_last_crc = crc;
   sram_have_crc = 1;
   return 1;
}

int fe_host_sram_flush(int force_write)
{
   /* Threaded: the writer thread owns the scan AND the writes, so this only
    * places the request.  A forced flush additionally waits for the whole
    * cycle — scan, then drain — to finish before returning, which is what
    * exit and the round-trip tests depend on. */
   if (host_io)
   {
      sram_scan_req = force_write ? 2 : 1;
      host_io->wake();
      if (force_write)
         fe_host_sram_sync();
      return 1;
   }

   /* Synchronous (desktop, or sram_thread=0): ADR-0020's behaviour exactly.
    * Finish anything already in flight before re-scanning, so a forced
    * flush is always complete when it returns. */
   if (sram_pending() || sram_full_req)
      (void)sram_drain(0);
   if (!sram_scan(force_write))
      return 0;
   if (sram_drain(force_write ? 0 : SRAM_FLUSH_BUDGET_US) != 0)
      return -1;
   return 1;
}

/* ----------------------------------------------------------------- boot -- */

int fe_host_option_set(const char *key, const char *value)
{
   size_t i;
   for (i = 0; i < sizeof(options) / sizeof(options[0]); i++)
   {
      if (strcmp(options[i].key, key) == 0)
      {
         strncpy(options[i].value, value, sizeof(options[i].value) - 1);
         options[i].value[sizeof(options[i].value) - 1] = '\0';
         return 0;
      }
   }
   return -1;
}

int fe_host_option_set_live(const char *key, const char *value)
{
#ifdef USE_PSP_RGB565_FORMAT
   /* ADR-0039: gba_cc_lut is baked in libretro channel order and this build
    * is not.  Refuse loudly here rather than render wrong colours later. */
   if (strcmp(key, "gpsp_color_correction") == 0 &&
       strcmp(value, "disabled") != 0)
   {
      fe_evt("option_refused key=%s reason=pixfmt_psp5650", key);
      return -1;
   }
#endif
   if (fe_host_option_set(key, value) != 0)
      return -1;
   options_dirty = 1;
   return 0;
}

static void log_bios_evt(void)
{
   char path[512];
   FILE *f;
   static uint8_t bios[16384];
   size_t n;
   char sha[41];

   snprintf(path, sizeof(path), "%s/gba_bios.bin",
            host.system_dir ? host.system_dir : ".");
   f = fopen(path, "rb");
   if (!f)
   {
      fe_evt("bios=missing");
      return;
   }
   n = fread(bios, 1, sizeof(bios), f);
   fclose(f);
   fe_sha1_hex(bios, n, sha);
   fe_evt("bios=%s sha1=%s",
          (n == 16384 && strcmp(sha, GBA_BIOS_SHA1) == 0) ? "real" : "unknown",
          sha);
}

static void log_rom_evt(void)
{
   FILE *f = fopen(host.rom_path, "rb");
   uint8_t code[5] = { 0 };
   long size = 0;

   if (f)
   {
      fseek(f, 0, SEEK_END);
      size = ftell(f);
      fseek(f, 0xAC, SEEK_SET);
      if (fread(code, 1, 4, f) != 4)
         memset(code, 0, sizeof(code));
      fclose(f);
   }
   fe_evt("rom_loaded code=%c%c%c%c size=%ld",
          code[0] ? code[0] : '?', code[1] ? code[1] : '?',
          code[2] ? code[2] : '?', code[3] ? code[3] : '?', size);
}

int fe_host_boot(const fe_host_config *cfg)
{
   struct retro_game_info game;
   struct retro_system_av_info av;

   host = *cfg;
   frame_count = 0;
   last_frame = NULL;

   retro_set_environment(env_cb);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_sample_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);

   retro_init();

   log_bios_evt();

   memset(&game, 0, sizeof(game));
   game.path = host.rom_path;
   if (!retro_load_game(&game))
   {
      fe_log("retro_load_game FAILED for %s", host.rom_path);
      retro_deinit();
      return -1;
   }

   log_rom_evt();

   retro_get_system_av_info(&av);
   /* ADR-0028: the ONE place the audio input rate is decided. Platforms read
    * it back with fe_host_sample_rate() after boot — audio_start() runs
    * before the core exists, so anything that latched a literal at thread
    * start would silently resample at the wrong ratio the moment
    * gpsp_sound_rate changed (double-speed playback + a draining ring). */
   core_sample_rate = (unsigned)av.timing.sample_rate;
   fe_evt("av_info fps=%.4f rate=%.0f w=%u h=%u",
          av.timing.fps, av.timing.sample_rate,
          av.geometry.base_width, av.geometry.base_height);

   sram_load();
   fps_last_us = host.time_us ? host.time_us() : 0;
   return 0;
}

/* EVT fps emu=A.AA rendered=B.BB skipped=N win=F dt_us=D
 *
 * Emitted on the heartbeat cadence (ADR-0019).  `emu` is retro_run/s — the
 * rate the game and the ARQ pump actually advance at.  `rendered` is what
 * reached the screen.  A gap between them is frameskip, and frameskip that
 * is not paid for by a higher `emu` is pure user-visible loss.  Integer
 * hundredths: no float formatting on the hot-ish path (and newlib-nano on
 * PSP would print `%f` as nothing at all). */
static void fps_evt(void)
{
   uint64_t now = host.time_us ? host.time_us() : 0;
   uint64_t dt  = (now > fps_last_us) ? now - fps_last_us : 0;
   unsigned demu = frame_count     - fps_last_frames;
   unsigned dren = frames_rendered - fps_last_rendered;
   unsigned dskp = frames_skipped  - fps_last_skipped;
   unsigned emu_x100 = 0, ren_x100 = 0;

   if (dt >= 1000)                   /* need a sane window to divide by */
   {
      emu_x100 = (unsigned)((uint64_t)demu * 100000000ull / dt);
      ren_x100 = (unsigned)((uint64_t)dren * 100000000ull / dt);
   }
   fe_evt("fps emu=%u.%02u rendered=%u.%02u skipped=%u win=%u dt_us=%llu",
          emu_x100 / 100, emu_x100 % 100, ren_x100 / 100, ren_x100 % 100,
          dskp, demu, (unsigned long long)dt);

   fps_last_us       = now;
   fps_last_frames   = frame_count;
   fps_last_rendered = frames_rendered;
   fps_last_skipped  = frames_skipped;
}

/* ADR-0027 §measurement — bracket retro_run itself.
 *
 * ADR-0021 priced every part of a session EXCEPT the core: pump 208, rx 186,
 * arq 32, enq 111 us, ~500 us of transport in total, against a client role
 * that costs ~12 fps versus the host role on both consoles.  That arithmetic
 * says the bulk of the client's extra cost is inside retro_run — the core's
 * RFU client path — but "says" is not "measures", and the same number also
 * bounds the still-open ~41-49 ms `frame` maximum: if `core_max` tracks
 * `frame_max` the residue is the dynarec/core, if it does not the residue is
 * ours (the GU blit / sceGuSync path, or the once-per-60-frames
 * sceIoGetstat(DUMP_MARKER) on a stick whose 4 KiB write costs 34 ms).
 * Two clock reads per frame on top of ADR-0021's ~13. */
static uint64_t core_run_us_total;
static uint32_t core_run_max_us;
static unsigned core_run_calls;

/* ADR-0028 — attribute the spike instead of guessing at it.
 *
 * The field settled ADR-0027's open question the other way from the rig: on
 * hardware `core` max is 21-29 ms against a 16.75 ms budget, and the 21 ms
 * spike appears BEFORE any peer connects — solo Emerald, no wireless. So the
 * residue is intrinsic to the core, and the two candidates that can cost tens
 * of milliseconds are (a) a translation-cache flush, which discards up to
 * 2 MiB of generated code on the SMALL_TRANSLATION_CACHE build and forces
 * re-translation, and (b) a ROM page fault, which is a 32 KiB read from the
 * memory stick mid-emulation whenever the ROM does not fit in RAM — on a
 * stick ADR-0026 measured at 34 ms for 4 KiB.
 *
 * Counting per frame is not enough: an average hides a once-a-minute event.
 * So we snapshot the counter deltas of the frame that SET the maximum. One
 * log line then names the cause, or rules both out. */
static unsigned core_spike_rom_flush, core_spike_ram_full, core_spike_ram_smc;
static unsigned core_spike_ram_dma, core_spike_page_load;

/* ...and the same again for the worst frame in the CURRENT window.
 *
 * The all-time snapshot alone is a trap, and the rig caught it: `core_run_max`
 * is typically set during boot, so its snapshot latches on an early frame and
 * reads `0/0/0` forever — even in a window that logged 4110 RAM flushes. A
 * stuck snapshot would have made the field log inconclusive, which is the one
 * thing this build exists to avoid. The window max is reset every heartbeat,
 * so it always describes work happening NOW. */
static uint32_t core_win_max_us;
static unsigned core_win_spike_rom, core_win_spike_full, core_win_spike_smc;
static unsigned core_win_spike_dma, core_win_spike_page;

unsigned fe_host_sample_rate(void)
{
   return core_sample_rate;
}

void fe_host_core_prof(unsigned *calls, uint32_t *total_us, uint32_t *max_us)
{
   if (calls)    *calls    = core_run_calls;
   if (total_us) *total_us = (uint32_t)core_run_us_total;
   if (max_us)   *max_us   = core_run_max_us;
}

void fe_host_core_spike(uint32_t *max_us, unsigned *rom_flush,
                        unsigned *ram_full, unsigned *ram_smc,
                        unsigned *ram_dma, unsigned *page_load)
{
   if (max_us)    *max_us    = core_run_max_us;
   if (rom_flush) *rom_flush = core_spike_rom_flush;
   if (ram_full)  *ram_full  = core_spike_ram_full;
   if (ram_smc)   *ram_smc   = core_spike_ram_smc;
   if (ram_dma)   *ram_dma   = core_spike_ram_dma;
   if (page_load) *page_load = core_spike_page_load;
}

/* ADR-0028: `EVT core_prof` on the heartbeat cadence, session or not.
 *
 * The decisive field fact was that the core's 21 ms spike appears with NO
 * wireless at all — solo Emerald, before any peer connects — so it cannot be
 * chased from `EVT sess_cost`, which only exists while a session is live.
 * This line is the solo-play equivalent:
 *   core=mean/winmax/max  us inside retro_run: mean over this window / worst
 *                         in this window / worst ever
 *   win=rom/full/smc/dma/page  counter deltas over this window
 *   wspike=…              what the core was doing on the worst frame IN THIS
 *                         WINDOW — the field to read first
 *   spike=…               ...and on the worst frame ever seen
 * ADR-0029 splits the old single `ram` field into `full` (RAM JIT cache
 * exhausted — a bigger RAM_TRANSLATION_CACHE_SIZE is the lever), `smc` (a
 * CPU store hit RAM holding translated code) and `dma` (a DMA did). The old
 * field could not tell them apart and they have different fixes: for the
 * last two a bigger cache is useless, and only the DMA case knows its whole
 * destination range up front.
 * A nonzero `wspike` field names the cause outright. All zeros, with a large
 * `winmax`, rules out both the translation-cache flush and ROM paging, and
 * the hunt moves inside the emulation loop itself. */
static void core_prof_evt(void)
{
   static unsigned prev_calls;
   static uint64_t prev_us;
   static unsigned prev_rf, prev_ff, prev_sf, prev_df, prev_pl;
   unsigned d = core_run_calls - prev_calls;
   unsigned rf = 0, ff = 0, sf = 0, df = 0, pl = 0;
   unsigned srf, sff, ssf, sdf, spl;

   if (host.core_counters)
      host.core_counters(&rf, &ff, &sf, &df, &pl);
   fe_host_core_spike(NULL, &srf, &sff, &ssf, &sdf, &spl);

   fe_evt("core_prof core=%u/%u/%u win=%u/%u/%u/%u/%u "
          "wspike=%u/%u/%u/%u/%u spike=%u/%u/%u/%u/%u",
          d ? (unsigned)((core_run_us_total - prev_us) / d) : 0u,
          core_win_max_us, core_run_max_us,
          rf - prev_rf, ff - prev_ff, sf - prev_sf, df - prev_df,
          pl - prev_pl,
          core_win_spike_rom, core_win_spike_full, core_win_spike_smc,
          core_win_spike_dma, core_win_spike_page,
          srf, sff, ssf, sdf, spl);

   prev_calls = core_run_calls;
   prev_us    = core_run_us_total;
   prev_rf = rf; prev_ff = ff; prev_sf = sf; prev_df = df; prev_pl = pl;
   /* Re-base the window max so the next line describes the next window. */
   core_win_max_us    = 0;
   core_win_spike_rom = core_win_spike_full = 0;
   core_win_spike_smc = core_win_spike_dma = core_win_spike_page = 0;
}

/* ADR-0030: `EVT smc_addr` beside `EVT core_prof`, same cadence.
 *
 *   EVT smc_addr win=<hits> pages=<distinct> iw=<n> ew=<n> oth=<n> ovf=<n>
 *                hot=<page>:<n>,...  top=<exact addr>:<n>,...
 *
 * `win` must EQUAL `core_prof`'s `win=.../smc` + `.../dma`: one bucket hit per
 * flush, so a mismatch means a write path is unprofiled or double-counted.  A
 * DMA is deliberately recorded once, at its first tagged halfword, because the
 * whole transfer costs one flush however many halfwords it crosses -- counting
 * per halfword would swamp the CPU-store events (one code DMA is ~1900 writes
 * but one flush).  How to read it:
 *   `hot` (256-byte pages)  few, clustered, high counts => the writes are
 *     concentrated; many scattered pages with low counts => they are not.
 *   `top` (exact addresses) settles what `hot` cannot: a single address with
 *     essentially all the hits is ONE instruction writing ONE location that
 *     happens to sit inside a tagged code range -- over-tagged data, and block
 *     termination is the cheap lever.  A spread of addresses inside one page
 *     is a real code copy, and only selective invalidation helps.
 *   `ovf` counts events that had to evict a heavy-hitter slot: near 0 means
 *     `top` is the whole story, large means the addresses are churning.
 * `oth` must be 0: it counts events whose region the store stub could not
 * attribute, and any nonzero value invalidates iw/ew and the addresses.
 * The whole line costs one pass over 1152 halfwords, once per 600 frames. */
#define SMC_ADDR_HOT  4

static int smc_addr_fmt(char *buf, size_t len, const unsigned *addr,
                        const unsigned *cnt)
{
   int n = 0;
   unsigned i;
   for (i = 0; i < SMC_ADDR_HOT && cnt[i]; i++)
      n += snprintf(buf + n, len - n, "%s%08x:%u", n ? "," : "",
                    addr[i], cnt[i]);
   if (!n)
      n = snprintf(buf, len, "-");
   return n;
}

static void smc_addr_evt(void)
{
   unsigned hits = 0, pages = 0, iw = 0, ew = 0, oth = 0, ovf = 0;
   unsigned ha[SMC_ADDR_HOT], hc[SMC_ADDR_HOT];
   unsigned ta[SMC_ADDR_HOT], tc[SMC_ADDR_HOT];
   char hot[SMC_ADDR_HOT * 20], top[SMC_ADDR_HOT * 20];

   if (!host.smc_addr)
      return;
   host.smc_addr(&hits, &pages, &iw, &ew, &oth, &ovf, ha, hc, ta, tc,
                 SMC_ADDR_HOT);

   smc_addr_fmt(hot, sizeof(hot), ha, hc);
   smc_addr_fmt(top, sizeof(top), ta, tc);

   fe_evt("smc_addr win=%u pages=%u iw=%u ew=%u oth=%u ovf=%u hot=%s top=%s",
          hits, pages, iw, ew, oth, ovf, hot, top);
}

/* Phase 5g: `EVT smc_block`, same cadence, the follow-on to `EVT smc_addr`.
 *
 *   EVT smc_block watch=<addr> xlat=<RAM blocks translated> ovf=<n> wovf=<n>
 *                 blk=<start>:<end>:<a|t>:<reason>:<count>,...
 *                 wr=<GBA PC>:<count>,...
 *   EVT smc_code  head@<start> <16 words>
 *   EVT smc_code  ctx@<addr>   <16 words>
 *
 * `blk` is every translated RAM block whose tagged range [start,end) covered
 * the watched address in this window -- i.e. every block whose translation
 * ARMED the tag the game then writes.  `reason` is why scan_block stopped:
 * 1 unconditional branch, 2 MAX_EXITS, 3 translation gate, 4 MAX_BLOCK_SIZE,
 * 5 the 0x3007FF0 end-of-IWRAM clamp.  Reasons 4 and 5 mean the scanner ran
 * out of room rather than off the end of a function, which is the signature of
 * translation running past the real end of code.  `wr` is the GBA PC of the
 * storing instruction (+4 ARM / +2 Thumb), which says whether the writer is an
 * IRQ prologue, a stack push, or an ordinary variable store.  `smc_code` is a
 * ONE-SHOT dump (first covering block seen, never re-taken) of the words at
 * the block head and around the watched address, for offline disassembly. */
#define SMC_BLK_FMT_MAX 24

static void smc_block_evt(void)
{
   unsigned watch = 0, xlat = 0, ovf = 0, wovf = 0, snap = 0;
   unsigned fus = 0, fbytes = 0, fwide = 0;
   unsigned same = 0, diff = 0, unkn = 0;
   unsigned start[FE_SMC_BLK_SLOTS], end[FE_SMC_BLK_SLOTS];
   unsigned mode[FE_SMC_BLK_SLOTS], reason[FE_SMC_BLK_SLOTS];
   unsigned cnt[FE_SMC_BLK_SLOTS];
   unsigned wpc[FE_SMC_WR_SLOTS], waddr[FE_SMC_WR_SLOTS], wcnt[FE_SMC_WR_SLOTS];
   unsigned head[FE_SMC_BLK_SNAP], ctx[FE_SMC_BLK_SNAP];
   char blks[FE_SMC_BLK_SLOTS * 40], wrs[FE_SMC_WR_SLOTS * 32];
   char words[FE_SMC_BLK_SNAP * 10];
   static int snap_logged;
   int n;
   unsigned i;

   if (!host.smc_block)
      return;
   host.smc_block(&watch, &xlat, &ovf, start, end, mode, reason, cnt,
                  wpc, waddr, wcnt, &wovf, head, ctx, &snap,
                  &fus, &fbytes, &fwide, &same, &diff, &unkn);

   n = 0;
   for (i = 0; i < FE_SMC_BLK_SLOTS && cnt[i]; i++)
      n += snprintf(blks + n, sizeof(blks) - n, "%s%08x:%08x:%c:%u:%u",
                    n ? "," : "", start[i], end[i], mode[i] ? 't' : 'a',
                    reason[i], cnt[i]);
   if (!n)
      snprintf(blks, sizeof(blks), "-");

   n = 0;
   for (i = 0; i < FE_SMC_WR_SLOTS && wcnt[i]; i++)
      n += snprintf(wrs + n, sizeof(wrs) - n, "%s%08x@%08x:%u",
                    n ? "," : "", wpc[i], waddr[i], wcnt[i]);
   if (!n)
      snprintf(wrs, sizeof(wrs), "-");

   fe_evt("smc_block watch=%08x xlat=%u ovf=%u wovf=%u fus=%u fkb=%u fwide=%u"
          " sil=%u/%u/%u blk=%s wr=%s",
          watch, xlat, ovf, wovf, fus, fbytes >> 10, fwide,
          same, diff, unkn, blks, wrs);

   if (snap && !snap_logged)
   {
      snap_logged = 1;
      n = 0;
      for (i = 0; i < FE_SMC_BLK_SNAP; i++)
         n += snprintf(words + n, sizeof(words) - n, "%s%08x",
                       i ? " " : "", head[i]);
      fe_evt("smc_code head@%08x %s", snap & ~1u, words);
      n = 0;
      for (i = 0; i < FE_SMC_BLK_SNAP; i++)
         n += snprintf(words + n, sizeof(words) - n, "%s%08x",
                       i ? " " : "", ctx[i]);
      fe_evt("smc_code ctx@%08x %s", watch & ~3u, words);
   }
}

/* Phase 5h: `EVT core_phase`, same cadence, the successor to `EVT smc_block`.
 *
 *   EVT core_phase lvl=<0-3> clk=<ns per clock read> f=<frames> rd=<reads/frame>
 *                  tot=<mean>/<max> cpu=… vid=… blt=… amix=… aout=… dsnd=…
 *                  jit=… rfu=… oth=…
 *                  worst=cpu:N,vid:N,blt:N,amix:N,aout:N,dsnd:N,jit:N,rfu:N,oth:N
 *                  cnt=<update_gba>/<dma>/<sound_timer> bk=<rd>/<wr>
 *                  wbk=<rd>/<wr> neg=<n>
 *
 * How to read it, in order:
 *  - `tot` must track `EVT core_prof`'s `core=` (same call, two independent
 *    clocks and two independent maxima).  A large disagreement means one of
 *    the two brackets is wrong, and neither should then be trusted.
 *  - `worst=` is the attribution.  The per-phase `max` values each come from
 *    whichever frame was worst FOR THAT PHASE, so they are not required to
 *    sum to `tot`'s max and must never be added up; `worst=` is the single
 *    frame that set `tot`'s max, broken down, and its parts DO sum to it.
 *  - `cpu` and `oth` are residues (see main.h).  `neg=<cpu>/<oth>` counts the
 *    frames where each came out negative.  A handful of `oth` events is the
 *    1 us clock quantisation showing through a residue that is only ~3 us
 *    wide (five brackets, each truncated independently); a nonzero `cpu`
 *    count, or an `oth` count that is not a rounding-scale number, means the
 *    brackets overlap and the partition must not be trusted.
 *  - `rd x clk` is what this line cost per frame.  At level 2 that is ~350
 *    reads, most of them inside `vid`; run level 1 for the same `tot` with
 *    ~10 reads to price the probe by difference instead of by assertion.
 *  - `bk` is backup-memory (flash/EEPROM/SRAM) CALLS, not microseconds --
 *    counted because the game polls flash thousands of times a frame and a
 *    clock read per call would cost more than the path.  `wbk` is the same
 *    two counts on the worst frame, which is what says whether a flash burst
 *    is implicated in a spike. */
static void core_phase_evt(void)
{
   static const char *nm[FE_CPH_NPHASE] = {
      "tot", "cpu", "vid", "blt", "amix", "aout", "dsnd", "jit", "rfu", "oth"
   };
   unsigned lvl = 0, clk = 0, frames = 0, reads = 0;
   unsigned mean[FE_CPH_NPHASE], max[FE_CPH_NPHASE], worst[FE_CPH_NPHASE];
   unsigned bkr = 0, bkw = 0, wbkr = 0, wbkw = 0;
   unsigned ugba = 0, dman = 0, stmr = 0, negc = 0, nego = 0;
   unsigned rfux = 0, rfut = 0, wrfux = 0, wrfut = 0;
   char times[FE_CPH_NPHASE * 26], wst[FE_CPH_NPHASE * 16];
   int n;
   unsigned i;

   if (!host.core_phase)
      return;
   host.core_phase(&lvl, &clk, &frames, &reads, mean, max, worst,
                   FE_CPH_NPHASE, &bkr, &bkw, &wbkr, &wbkw,
                   &ugba, &dman, &stmr, &negc, &nego,
                   &rfux, &rfut, &wrfux, &wrfut);
   if (!lvl || !frames)
      return;

   n = 0;
   for (i = 0; i < FE_CPH_NPHASE; i++)
      n += snprintf(times + n, sizeof(times) - n, "%s%s=%u/%u",
                    i ? " " : "", nm[i], mean[i], max[i]);

   n = 0;
   for (i = FE_CPH_CPU; i < FE_CPH_NPHASE; i++)
      n += snprintf(wst + n, sizeof(wst) - n, "%s%s:%u",
                    i > FE_CPH_CPU ? "," : "", nm[i], worst[i]);

   /* ADR-0051: rfux is rfu_transfer() calls/frame and rfut the us they
    * cost -- both charged to `cpu`, not to the `rfu` phase. wrfu* are the
    * same two on the single worst-`tot` frame. */
   fe_evt("core_phase lvl=%u clk=%u f=%u rd=%u %s worst=%s "
          "cnt=%u/%u/%u bk=%u/%u wbk=%u/%u neg=%u/%u rfux=%u/%u wrfu=%u/%u",
          lvl, clk, frames, reads, times, wst,
          ugba, dman, stmr, bkr, bkw, wbkr, wbkw, negc, nego,
          rfux, rfut, wrfux, wrfut);
}

void fe_host_run_frame(void)
{
   uint64_t core_t0 = host.time_us ? host.time_us() : 0;
   unsigned rf0 = 0, ff0 = 0, sf0 = 0, df0 = 0, pl0 = 0;

   if (core_t0 && host.core_counters)
      host.core_counters(&rf0, &ff0, &sf0, &df0, &pl0);

   retro_run();

   if (core_t0)
   {
      uint32_t d = (uint32_t)(host.time_us() - core_t0);
      core_run_us_total += d;
      core_run_calls++;
      if (d > core_run_max_us || d > core_win_max_us)
      {
         unsigned rf1 = 0, ff1 = 0, sf1 = 0, df1 = 0, pl1 = 0;
         if (host.core_counters)
            host.core_counters(&rf1, &ff1, &sf1, &df1, &pl1);
         if (d > core_run_max_us)
         {
            core_run_max_us      = d;
            core_spike_rom_flush = rf1 - rf0;
            core_spike_ram_full  = ff1 - ff0;
            core_spike_ram_smc   = sf1 - sf0;
            core_spike_ram_dma   = df1 - df0;
            core_spike_page_load = pl1 - pl0;
         }
         if (d > core_win_max_us)
         {
            core_win_max_us     = d;
            core_win_spike_rom  = rf1 - rf0;
            core_win_spike_full = ff1 - ff0;
            core_win_spike_smc  = sf1 - sf0;
            core_win_spike_dma  = df1 - df0;
            core_win_spike_page = pl1 - pl0;
         }
      }
   }
   frame_count++;

   /* Keep an in-progress save write moving (ADR-0020) — a few 4 KiB blocks
    * per frame under a wall-clock budget, never a 128 KiB stall.  Both of
    * these are no-ops once a writer thread is installed: since ADR-0026 it
    * owns the scan cadence as well as the writes, so the emulation thread
    * pays exactly nothing for the save path. */
   fe_host_sram_service();
   if (!host_io && (frame_count % SRAM_CHECK_INTERVAL) == 0)
      fe_host_sram_flush(0);
   if ((frame_count % HEARTBEAT_INTERVAL) == 0)
   {
      fe_evt("heartbeat frames=%u t_us=%llu", frame_count,
             (unsigned long long)(host.time_us ? host.time_us() : 0));
      fps_evt();
      core_prof_evt();
      smc_addr_evt();
      smc_block_evt();
      core_phase_evt();
   }
}

void fe_host_frame_stats(unsigned *emulated, unsigned *rendered,
                         unsigned *skipped)
{
   if (emulated) *emulated = frame_count;
   if (rendered) *rendered = frames_rendered;
   if (skipped)  *skipped  = frames_skipped;
}

void fe_host_audio_buffer_status(int active, int occupancy_pct,
                                 int underrun_likely)
{
   if (!audio_buf_status_cb)
      return;                    /* core is not running an auto frameskip */
   if (occupancy_pct < 0)   occupancy_pct = 0;
   if (occupancy_pct > 100) occupancy_pct = 100;
   audio_buf_status_cb(active ? true : false, (unsigned)occupancy_pct,
                       underrun_likely ? true : false);
}

unsigned fe_host_frame_count(void)
{
   return frame_count;
}

const uint16_t *fe_host_last_frame(size_t *pitch_bytes)
{
   if (pitch_bytes)
      *pitch_bytes = last_pitch;
   return last_frame;
}

const void *fe_host_netpacket_cb(void)
{
   return netpacket_registered ? (const void *)&netpacket_cb : NULL;
}

void fe_host_shutdown(void)
{
   /* ADR-0025 ordering contract: the writer thread is already stopped and
    * fe_host_set_io(NULL) already called, so everything below is
    * single-threaded — which is what makes it safe to close the file and
    * then let retro_unload_game() free the buffer the writer was reading. */
   fe_host_sram_flush(0);
   fe_host_sram_sync();     /* exit must never close over pending blocks */
   sram_close();
   retro_unload_game();
   retro_deinit();
}
