#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "libretro.h"
#include "../gbcore/gbcore.h"
#include "../gbcore/gbcore_dual.h"
#include "fe_gblink.h"
#include "fe_linkprof.h"
#include "../netdrv/gb_link.h"
#include "netpacket_host.h"
#ifdef REWIND
#include "fe_rewind.h"
#endif
#ifdef GBA_LINK
#include "fe_gbadual.h"
#include <stdarg.h>
static void gbadual_log(const char *fmt, ...)
{
   char buf[320];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   fe_evt("%s", buf);
   (void)buf;
}
static void gbadual_evt(const char *fmt, ...)
{
   char buf[400];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   fe_evt("%s", buf);
   (void)buf;
}
#endif

/* SHA-1 of the official GBA BIOS (Nintendo AGB BIOS, 16 KiB). */
#define GBA_BIOS_SHA1 "300c20df6731a33952ded8c436f7f186d25d3492"
#define SRAM_CHECK_INTERVAL 300   /* frames ~= 5 s (FRONTEND-AUDIT §5) */
/* Frames between telemetry heartbeats.  600 is ~10 s at full speed, but a
 * game running at 24 fps stretches that to 25 s — a short diagnostic run can
 * exit before the FIRST heartbeat and produce a log with no measurements in
 * it at all.  Overridable at build time (-DHEARTBEAT_INTERVAL=N) so a
 * profiling EBOOT can sample much more often. */
#ifndef HEARTBEAT_INTERVAL
#define HEARTBEAT_INTERVAL  600
#endif
/* The cadence in force: fe_host_config.heartbeat_frames when set (harness
 * `telemetry_period`, for timing runs that must not pay for their own
 * reporting), else HEARTBEAT_INTERVAL. */
static unsigned hb_every = HEARTBEAT_INTERVAL;

/* ---------------------------------------------------------------- state -- */

static fe_host_config host;
static gbcore_t *gb_core;
/* A machine pair (host.gb_dual_rom): gb_core is then its slot 0. */
static gbdual_t *gb_dual;
static uint16_t gb_dual_buttons;
/* A GB/GBC session, single-player or linked: the GB paths below key on this,
 * not on gb_core, because a link session (gbl) runs with no single core. */
static int gb_family;
/* The GB link session (fe_gblink.h), NULL when none. */
static int save_recover_bak(const char *path);   /* below, with sram_load */
static int state_file_exists(const char *path);
static int state_file_rename(const char *from, const char *to);
static int state_file_replace(const char *path, const void *data, size_t size);
/* A failed battery load is not a new cartridge. Boot refuses it, and even
 * cleanup/exit must not persist its empty or partially read RAM. Cleared by
 * the next successful load (including a genuinely absent save). */
static int battery_load_failed;
static fe_gblink *gbl;
static int gbl_last;               /* last fe_gblink_step result */
static int gbl_is_gba;             /* the session is the GBA cable mirror */
static int gbl_ride_set;           /* Fast link: ack riding applied */
static int gbl_primed;             /* autopilot had its power-on look */
static uint8_t *gbl_prelink_state; /* the running game when the link began */
static size_t gbl_prelink_state_n;
static int gbl_hold;               /* harness: single machine held for a link */
/* GBA link-cable session (GBA_LINK builds, host.gba_dual_session): the
 * player's machine is instance A, which keeps running the frontend's core;
 * its battery image is the session's to commit, so the writer is held. */
#ifdef GBA_LINK
static int gba_cable;
static int gba_rom_sampled = 1;  /* fe_host_gba_cable_rom_sampled */
static volatile int gba_sram_hold;
static volatile int gba_sram_quiescing;
static uint8_t *gba_prelink_sram;
#define GBA_CABLE gba_cable
#else
#define GBA_CABLE 0            /* release: none of this exists */
#endif
static unsigned gbl_sessions;      /* link sessions closed since boot */
static uint8_t *gb_save_buffer;
static size_t gb_save_capacity;
static uint32_t gb_save_crc;
static int gb_save_crc_valid;
/* The periodic GB flush hands its snapshot to the writer thread (see
 * gb_save_flush_periodic): a second buffer the emulation thread never
 * touches while gb_io_req is set. */
static uint8_t *gb_io_buffer;
static size_t gb_io_capacity, gb_io_size;
static uint32_t gb_io_crc;
static volatile int gb_io_req;       /* 1: gb_io_buffer is the writer's */
static volatile int gb_io_failed;    /* the writer's last write failed */
static const fe_host_io *host_io;    /* defined with the GBA writer below */
static unsigned frame_count;         /* likewise, with the frame loop */
static uint32_t gb_seen_crc;         /* cart RAM at the previous check */
static unsigned gb_dirty_since;      /* frame it first differed; 0 = clean */
static gb_link_t gb_link;
typedef struct { uint8_t peer_id; } gb_link_send_context;
static gb_link_send_context gb_link_send_ctx;
static uint16_t gb_link_next_transfer;
static uint8_t gb_link_received_byte;
static enum gb_link_result gb_link_result;
static int gb_link_initialized;
static int gb_link_completed;
static const uint16_t *last_frame;
static size_t last_pitch;
static unsigned frames_rendered;
static unsigned frames_skipped;   /* ADR-0019 accounting; see below */

/* The core renders straight into the frontend's 16-bit layout (gbcore.h),
 * so its frame is handed on as-is: no conversion pass and no copy.  The
 * buffer stays unchanged until the next gbcore_run_frame, which is as long
 * as fe_host_last_frame() consumers (dumps, state thumbnails, the wake
 * overlay) run; shutdown clears last_frame before the core frees it. */
static int gb_skip_frame;   /* this frame's drawing was skipped (frameskip) */
/* fe_host_skip_override(): -1 none.  The GBA core reads its own copy; weak,
 * so host tests that link this file without the core still link. */
static int skip_override = -1;
extern int frontend_skip_override __attribute__((weak));   /* libretro.c */

static void gb_video_frame(void *userdata, const gbcore_video_frame_t *frame)
{
   (void)userdata;
   if (gb_skip_frame)
   {
      /* Same contract as the GBA core's frameskip: NULL = nothing new. */
      frames_skipped++;
      if (host.video_frame)
         host.video_frame(NULL, 160, 144, last_pitch);
      return;
   }
   if (!frame || !frame->pixels || frame->width != 160 || frame->height != 144 ||
       frame->pitch_bytes < frame->width * sizeof(uint16_t))
      return;
   last_frame = frame->pixels;
   last_pitch = frame->pitch_bytes;
   frames_rendered++;
   if (host.video_frame)
      host.video_frame(frame->pixels, 160, 144, last_pitch);
}

static void gb_audio_batch(void *userdata, const int16_t *samples, size_t frames)
{
   (void)userdata;
   if (host.audio_frames)
      host.audio_frames(samples, frames);
}

/* The machine pair's buttons: the player's for slot 0, the same or none for
 * the headless partner (harness measurement; no network behind it). */
static int gb_dual_input(void *userdata, int slot, uint64_t frame,
                         uint16_t *buttons)
{
   (void)userdata;
   (void)frame;
   *buttons = (slot == 0 || host.gb_dual_mirror) ? gb_dual_buttons : 0;
   return 1;
}

static uint16_t gb_buttons_from_retro(uint32_t pad)
{
   uint16_t out = 0;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT))  out |= GBCORE_BUTTON_RIGHT;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT))   out |= GBCORE_BUTTON_LEFT;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_UP))     out |= GBCORE_BUTTON_UP;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN))   out |= GBCORE_BUTTON_DOWN;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_A))      out |= GBCORE_BUTTON_A;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_B))      out |= GBCORE_BUTTON_B;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_SELECT)) out |= GBCORE_BUTTON_SELECT;
   if (pad & (1u << RETRO_DEVICE_ID_JOYPAD_START))  out |= GBCORE_BUTTON_START;
   return out;
}

/* Room past cart RAM for any clock trailer the core accepts (largest 48 B). */
#define GB_SAVE_TRAILER_MAX 64

static long gb_save_file_len = -1;   /* bytes on disk; -1 = absent/unknown */

static int gb_save_reserve(size_t size)
{
   uint8_t *buffer;
   if (size <= gb_save_capacity)
      return 0;
   buffer = (uint8_t *)realloc(gb_save_buffer, size);
   if (!buffer)
      return -1;
   gb_save_buffer = buffer;
   gb_save_capacity = size;
   return 0;
}

/* ---- the clock trailer's stamp is UTC ----
 *
 * The 48-byte trailer of a clock cartridge's save ends in the UNIX time it
 * was written, which the format defines as UTC (bgb.bircd.org/rtcsave.html)
 * and VBA-M, BGB, SameBoy and mGBA all write as UTC.  Up to 3.5.0 the PSP
 * wrote its LOCAL time there, so a save carried to or from another emulator
 * was off by the player's UTC offset -- and, west of UTC, a save another
 * emulator had just written was "from the future" and the clock was reset.
 *
 * The core's clock (host.wallclock) is now UTC.  What remains is telling a
 * file the old releases wrote from every other file, since the two stamps
 * cannot be told apart by their value:
 *
 *   - every trailer written here is MARKED: "UTC" in the three unused upper
 *     bytes of the latched day-high register (trailer bytes 37-39).  The
 *     registers are bytes in 32-bit fields; VBA-M reads that field through
 *     a (uint8_t) cast, SameBoy declares those bytes padding, mGBA assigns
 *     the field to a uint8_t, and releases up to 3.5.0 cast it to a byte.
 *     A marked trailer is UTC, with no further question;
 *   - an unmarked trailer stamped at or before host.gb_rtc_legacy_before
 *     -- the moment this install first ran a release that stamps UTC, on the
 *     old clock -- is taken as an old release's, and its stamp is moved onto
 *     the UTC clock by the two clocks' present difference.  now - stamp is
 *     then exactly what 3.5.0 computed;
 *   - any other unmarked trailer is another emulator's: UTC.
 *
 * The middle rule also catches a file another emulator wrote before that
 * moment and the player imports afterwards; it is read as 3.5.0 read it
 * (off by the UTC offset).  Nothing in the file can tell the two apart. */
#define GB_RTC_TRAILER 48u
#define GB_RTC_TRAILER_32 44u
#define GB_RTC_EPOCH_MIN 852076800ll

/* The trailer of `image` (n bytes) for the running cartridge, or NULL. */
static uint8_t *gb_rtc_trailer(uint8_t *image, size_t n)
{
   size_t ram = gbcore_cart_ram_size(gb_core);
   if (gbcore_save_ram_size(gb_core) != ram + GB_RTC_TRAILER)
      return NULL;                        /* not a clock cartridge */
   if (n != ram + GB_RTC_TRAILER && n != ram + GB_RTC_TRAILER_32)
      return NULL;
   return image + ram;
}

static void gb_rtc_mark(uint8_t *t)
{
   t[37] = 'U'; t[38] = 'T'; t[39] = 'C';
}

static int gb_rtc_marked(const uint8_t *t)
{
   return t[37] == 'U' && t[38] == 'T' && t[39] == 'C';
}

/* Before the core sees a file: an old release's stamp onto the UTC clock. */
static void gb_rtc_from_legacy(uint8_t *image, size_t n)
{
   uint8_t *t = gb_rtc_trailer(image, n);
   int bytes = t && n - (size_t)(t - image) == GB_RTC_TRAILER ? 8 : 4;
   int64_t stamp = 0, now, old, ahead;
   int i;
   if (!t || !host.wallclock || !host.wallclock_legacy ||
       host.gb_rtc_legacy_before <= 0 || gb_rtc_marked(t))
      return;
   for (i = 0; i < bytes; i++)
      stamp |= (int64_t)t[40 + i] << (8 * i);
   if (stamp < GB_RTC_EPOCH_MIN || stamp > host.gb_rtc_legacy_before)
      return;
   now = (int64_t)host.wallclock();
   old = (int64_t)host.wallclock_legacy();
   if (now <= 0 || old <= 0)
      return;                             /* no reading: leave it be */
   /* The two readings are a moment apart; zones are whole minutes. */
   ahead = old - now;
   ahead = (ahead >= 0 ? ahead + 30 : ahead - 30) / 60 * 60;
   fe_evt("gb_rtc_legacy stamp=%lld ahead=%lld before=%lld",
          (long long)stamp, (long long)ahead, host.gb_rtc_legacy_before);
   stamp -= ahead;
   for (i = 0; i < bytes; i++)
      t[40 + i] = (uint8_t)((uint64_t)stamp >> (8 * i));
}

/* A clock cartridge's save that is cartridge RAM and nothing else -- a dump
 * of a real cartridge, RetroArch's .srm (its clock lives in a separate
 * .rtc), a flash cart's file.  The game's progress is there and the clock is
 * not, and the clock cannot be invented: started from zero, as it was up to
 * 3.5.0, the game showed a time that was simply wrong and said nothing.
 *
 * The cartridge has a way to say "the clock cannot be trusted": the day
 * counter's carry bit, which a real one sets when more than 511 days pass.
 * So such a file is completed with a trailer that says exactly that -- the
 * clock at zero, running, carry set -- stamped with the format's "written
 * without a clock" time, which adds nothing at load.  SameBoy does the same
 * (Core/gb.c, reset_rtc: "This gives the game a hint that the clock should
 * be reset").  Pokemon Gold, Silver and Crystal then show TIME NOT SET on
 * the menu, say "The clock's time may be wrong.  Please reset the time." on
 * CONTINUE and let the player set the day and time there; saving in the
 * game clears the bit (run on Crystal and Silver, see the commit).
 *
 * Only here, only for a file of exactly the cartridge's RAM size: not with
 * no file at all (a new game), not for any file with a trailer, never
 * inside the core (a link session's loads are untouched), and once -- the
 * first flush rewrites the file with a real trailer, whose flag is from then
 * on whatever the game leaves it.  Returns the size to load. */
static size_t gb_rtc_no_trailer(uint8_t *image, size_t n)
{
   size_t ram = gbcore_cart_ram_size(gb_core);
   uint8_t *t = image + ram;
   if (!ram || n != ram ||
       gbcore_save_ram_size(gb_core) != ram + GB_RTC_TRAILER)
      return n;
   memset(t, 0, GB_RTC_TRAILER);
   t[16] = 0x80;                          /* live day-high: the carry bit */
   t[36] = 0x80;                          /* and as last latched */
   memset(t + 40, 0xFF, 8);               /* 0x7fffffff7fffffff: no clock */
   t[43] = 0x7F;
   t[47] = 0x7F;
   fe_evt("gb_rtc_no_trailer size=%u: clock flagged for the game to reset",
          (unsigned)n);
   return ram + GB_RTC_TRAILER;
}

/* Change detection covers cart RAM only.  An RTC cart's battery image ends in
 * a clock trailer stamped with the wall clock at the moment it is read, so a
 * CRC over the whole image changed every second and rewrote the save every
 * SRAM_CHECK_INTERVAL for as long as Pokemon Gold/Silver/Crystal ran.  The
 * trailer on disk stays a consistent (clock, timestamp) pair from the last
 * write, which is all the next load needs to advance the clock. */
static uint32_t gb_save_ram_crc(size_t image_size)
{
   size_t ram = gbcore_cart_ram_size(gb_core);
   if (ram > image_size)
      ram = image_size;
   return fe_crc32(0, gb_save_buffer, ram);
}

/* Read the cartridge's battery image into gb_save_buffer (emulation thread
 * only: the core owns that RAM).  1 = it differs from what is on disk (or
 * force), 0 = nothing to write, -1 = error. */
static int gb_save_snapshot(int force, size_t *size_out, uint32_t *crc_out)
{
   size_t size = gbcore_save_ram_size(gb_core);
   if (battery_load_failed)
      return -1;
   if (!gb_core || !host.save_path || !size)
      return 0;
   if (gb_save_reserve(size) != 0 ||
       gbcore_save_ram_read(gb_core, gb_save_buffer, gb_save_capacity) != 0)
      return -1;
   *size_out = size;
   *crc_out = gb_save_ram_crc(size);
   if (!force && gb_save_crc_valid && *crc_out == gb_save_crc)
      return 0;
   {
      uint8_t *t = gb_rtc_trailer(gb_save_buffer, size);
      if (t)
         gb_rtc_mark(t);
   }
   return 1;
}

/* Preserve the previous file through short writes, full disks, deferred
 * close errors and renames.  Either thread; the caller owns `data`. */
static int gb_save_write(const uint8_t *data, size_t size, uint32_t crc)
{
   if (save_recover_bak(host.save_path) < 0 ||
       state_file_replace(host.save_path, data, size) != 0)
   {
      gb_save_file_len = -1;
      fe_evt("gb_sram_flush size=%u FAILED", (unsigned)size);
      return -1;
   }
   gb_save_file_len = (long)size;
   fe_evt("gb_sram_flush size=%u crc=%08x", (unsigned)size, crc);
   return 0;
}

/* Wait for a periodic write the writer thread still holds.  Never write the
 * file from here while it does: that would put two writers on one path. */
static int gb_io_settle(void)
{
   const fe_host_io *io = host_io;
   int spins;
   for (spins = 0; gb_io_req && io && spins < 4000; spins++)   /* ~4 s */
   {
      io->wake();
      io->yield();
   }
   if (gb_io_req)
   {
      fe_evt("gb_sram_sync_timeout");
      return -1;
   }
   if (gb_io_failed)
   {
      /* Its CRC was recorded when it was handed over; the file never got
       * it, so the next write must not be skipped as "unchanged". */
      gb_io_failed = 0;
      gb_save_crc_valid = 0;
   }
   return 0;
}

/* Synchronous: exit, sleep, quit, state and link paths.  Writes whatever
 * differs from the file now, on the calling (emulation) thread. */
static int gb_save_flush(int force)
{
   size_t size;
   uint32_t crc;
   int rc;
   if (gb_io_settle() != 0)
      return -1;
   rc = gb_save_snapshot(force, &size, &crc);
   if (rc <= 0)
      return rc;
   if (gb_save_write(gb_save_buffer, size, crc) != 0)
      return -1;
   gb_save_crc = crc;
   gb_save_crc_valid = 1;
   gb_dirty_since = 0;
   return 1;
}

/* Upper bound on how long a cartridge that never stops changing waits. */
#define GB_SAVE_MAX_DELAY (6 * SRAM_CHECK_INTERVAL)   /* ~30 s */

/* The every-SRAM_CHECK_INTERVAL flush during play.
 *
 * Pokemon Red/Blue/Gold/Silver/Crystal (and others) use battery RAM as
 * scratch -- sprite decompression -- so during ordinary play it differs at
 * almost every check, and writing on every difference meant a Memory Stick
 * rewrite every 5 s.  Write when the RAM has changed and then held still for
 * one interval (an in-game save lands 5-10 s later), or once it has been
 * different for GB_SAVE_MAX_DELAY.  Exit, sleep and quit flush at once
 * through gb_save_flush.
 *
 * The write itself goes to the writer thread when there is one: the
 * replacement is a temporary file, a close, renames and removes, each a FAT
 * update, and on the emulation thread that was a visible hitch. */
static int gb_save_flush_periodic(void)
{
   size_t size;
   uint32_t crc;
   int rc;
   if (gb_io_req)
      return 0;                     /* the previous write is still going */
   if (gb_io_failed)
   {
      gb_io_failed = 0;
      gb_save_crc_valid = 0;
   }
   rc = gb_save_snapshot(0, &size, &crc);
   if (rc <= 0)
   {
      if (rc == 0)
         gb_dirty_since = 0;
      return rc;
   }
   if (!gb_dirty_since || crc != gb_seen_crc)
   {
      gb_seen_crc = crc;
      if (!gb_dirty_since)
         gb_dirty_since = frame_count ? frame_count : 1;
      if (frame_count - gb_dirty_since < GB_SAVE_MAX_DELAY)
         return 0;                  /* still changing: wait for it to settle */
   }
   gb_dirty_since = 0;
   if (host_io && size > gb_io_capacity)
   {
      uint8_t *b = (uint8_t *)realloc(gb_io_buffer, size);
      if (b)
      {
         gb_io_buffer = b;
         gb_io_capacity = size;
      }
   }
   if (!host_io || size > gb_io_capacity)
   {
      /* No writer thread, or no memory for its copy: write here. */
      if (gb_save_write(gb_save_buffer, size, crc) != 0)
         return -1;
      gb_save_crc = crc;
      gb_save_crc_valid = 1;
      return 1;
   }
   memcpy(gb_io_buffer, gb_save_buffer, size);
   gb_io_size = size;
   gb_io_crc = crc;
   gb_save_crc = crc;               /* gb_io_failed undoes this */
   gb_save_crc_valid = 1;
   __asm__ __volatile__("" ::: "memory");
   gb_io_req = 1;
   host_io->wake();
   return 1;
}

/* Writer thread: the snapshot the emulation thread handed over. */
static int gb_save_service_io(void)
{
   if (!gb_io_req)
      return 0;
   __asm__ __volatile__("" ::: "memory");
   if (gb_save_write(gb_io_buffer, gb_io_size, gb_io_crc) != 0)
      gb_io_failed = 1;
   __asm__ __volatile__("" ::: "memory");
   gb_io_req = 0;
   return 1;
}

/* Loads whatever the file holds, like the GBA path does: cart RAM alone
 * (raw dumps), or RAM plus the 44/48-byte VBA-M/BGB clock trailer -- the
 * layout SameBoy wrote for the saves made before the TGB Dual core, too.
 * Requiring the exact size rejected saves imported from other emulators,
 * and the first periodic flush then overwrote the player's file with a
 * blank cartridge. */
static int gb_save_load(void)
{
   size_t size = gbcore_save_ram_size(gb_core);
   size_t cap = size + GB_SAVE_TRAILER_MAX, n, load;
   int whole, read_ok;
   FILE *f;
   gb_save_crc_valid = 0;
   gb_save_file_len = -1;
   battery_load_failed = 0;
   if (!size || !host.save_path)
      return 0;
   battery_load_failed = 1;
   if (gb_save_reserve(cap) != 0 || save_recover_bak(host.save_path) < 0)
      goto failed;
   f = fopen(host.save_path, "rb");
   if (!f)
   {
      if (errno != ENOENT)
         goto failed;
      fe_evt("gb_sram_load size=0");
      battery_load_failed = 0;
      return 0;
   }
   n = fread(gb_save_buffer, 1, cap, f);
   whole = fgetc(f) == EOF;
   read_ok = !ferror(f);
   if (fclose(f) != 0)
      read_ok = 0;
   if (!read_ok)
      goto failed;
   if (!n)
   {
      fe_evt("gb_sram_load size=0");
      battery_load_failed = 0;
      return 0;
   }
   /* Longer than any known layout: keep the RAM, let the clock reset. */
   load = whole ? n : gbcore_cart_ram_size(gb_core);
   if (load > n)
      load = n;
   if (whole)
   {
      gb_save_file_len = (long)n;
      load = gb_rtc_no_trailer(gb_save_buffer, load);
   }
   gb_rtc_from_legacy(gb_save_buffer, load);
   if (gbcore_save_ram_write(gb_core, gb_save_buffer, load) != 0 ||
       gbcore_save_ram_read(gb_core, gb_save_buffer, gb_save_capacity) != 0)
   {
      fe_evt("gb_sram_load FAILED size=%u", (unsigned)n);
      goto failed;
   }
   /* A file in another layout stays "dirty", so the first flush rewrites it
    * in the standard layout (RAM, then the 48-byte trailer on clock carts)
    * -- with the RAM just loaded, not a blank one. */
   gb_save_crc = gb_save_ram_crc(size);
   gb_save_crc_valid = gb_save_file_len == (long)size;
   fe_evt("gb_sram_load size=%u file=%u crc=%08x", (unsigned)size,
          (unsigned)n, gb_save_crc);
   battery_load_failed = 0;
   return 0;
failed:
   fe_log("gb_sram_load FAILED; preserving battery file %s", host.save_path);
   return -1;
}

/* Per-boot cable state.  Registered with netpacket before the core exists, so
 * it must be clean before those callbacks can fire. */
static void gb_link_reset(void)
{
   memset(&gb_link, 0, sizeof(gb_link));
   gb_link_send_ctx.peer_id = 0xFF;
   gb_link_next_transfer = 0;
   gb_link_received_byte = 0xFF;
   gb_link_result = GB_LINK_RESULT_OK;
   gb_link_completed = 0;
   gb_link_initialized = 0;
}

/* One bound for a GB link byte: the coordinator's protocol timeout and the
 * wall-clock cap on how long gb_serial_transfer holds the emulation thread. */
#define GB_LINK_TIMEOUT_MS 3000u

static uint32_t gb_link_now_ms(void)
{
   uint64_t now = host.time_us ? host.time_us() : 0;
   return (uint32_t)(now / 1000u);
}

static int gb_link_send_message(void *ctx,
                                const gb_link_message_t *message)
{
   gb_link_send_context *send_ctx = (gb_link_send_context *)ctx;
   uint8_t wire[GB_LINK_WIRE_SIZE];
   if (!send_ctx || gb_link_encode(message, wire) < 0)
      return -1;
   return fe_np_gb_send(send_ctx->peer_id, wire, sizeof(wire));
}

static void gb_link_complete_message(void *ctx, uint16_t transfer_id,
                                     uint8_t received,
                                     enum gb_link_result result)
{
   (void)ctx;
   (void)transfer_id;
   gb_link_received_byte = received;
   gb_link_result = result;
   gb_link_completed = 1;
}

/* Session messages that arrive before this console's session exists.  The
 * partner starts its session when ITS radio is up, and the ordered channel
 * delivers its HELLO at once -- if this console has not called
 * fe_host_gblink_start yet, that HELLO used to be dropped and never resent:
 * both sessions then waited forever (PPSSPP, consoles starting their
 * sessions 30 frames apart).  Only a HELLO is kept -- the partner sends
 * nothing else before ours reaches it, and a straggler from a session that
 * just ended must not reach the next one -- and replayed into the session
 * when it starts; a disconnect forgets it. */
#define GBL_EARLY_MAX 4
#define GBL_EARLY_LEN 128
static uint8_t gbl_early[GBL_EARLY_MAX][GBL_EARLY_LEN];
static size_t gbl_early_len[GBL_EARLY_MAX];
static unsigned gbl_early_n;

static void gb_link_receive_message(void *userdata, const void *payload,
                                    size_t len, uint8_t src_id)
{
   gb_link_message_t message;
   (void)userdata;
   if (gbl)
   {
      fe_gblink_receive(gbl, payload, len);
      return;
   }
   if (len != GB_LINK_WIRE_SIZE && (gb_family || GBA_CABLE))
   {
      if (gbl_early_n < GBL_EARLY_MAX && len == FE_GBLINK_HELLO_LEN &&
          ((const uint8_t *)payload)[0] == FE_GBLINK_HELLO_TYPE)
      {
         memcpy(gbl_early[gbl_early_n], payload, len);
         gbl_early_len[gbl_early_n++] = len;
         fe_evt("gblink_early_msg type=%u len=%u n=%u",
                len ? ((const uint8_t *)payload)[0] : 0u, (unsigned)len,
                gbl_early_n);
      }
      else
         fe_evt("gblink_early_drop len=%u", (unsigned)len);
      return;
   }
   if (len != GB_LINK_WIRE_SIZE ||
       gb_link_decode(&message, (const uint8_t *)payload, (unsigned)len) != 0 ||
       message.sender_id != src_id)
   {
      fe_evt("gb_link_drop reason=malformed len=%u src=%u", (unsigned)len,
             src_id);
      return;
   }
   gb_link_receive(&gb_link, &message, gb_link_now_ms());
}

static void gb_link_peer_event(void *userdata, uint8_t peer_id, int connected)
{
   uint8_t local_id;
   (void)userdata;
   if (gbl)
   {
      fe_gblink_peer(gbl, connected);
      return;
   }
   if (!connected)
      gbl_early_n = 0;
   if (GBA_CABLE)
      return;           /* no byte link: only the session uses the channel */
   if (connected)
   {
      if (gb_link_initialized && gb_link.peer_id != peer_id)
      {
         fe_evt("gb_link_peer_rejected reason=second_peer id=%u", peer_id);
         return;
      }
      if (fe_np_gb_local_id(&local_id) == 0)
      {
         fe_evt("gb_link_peer_error reason=no_local_id");
         return;
      }
      /* Same peer again (a re-JOIN resets its channel): finish any byte in
       * flight before gb_link_init wipes it, or the waiting SC write would
       * sit out the full timeout for a completion that can never come. */
      if (gb_link_initialized)
         gb_link_set_connected(&gb_link, 0);
      gb_link_send_ctx.peer_id = peer_id;
      gb_link_init(&gb_link, local_id, peer_id, GB_LINK_TIMEOUT_MS,
                   &gb_link_send_ctx,
                   gb_link_send_message, gb_link_complete_message);
      gb_link_initialized = 1;
      gb_link_set_connected(&gb_link, 1);
      fe_evt("gb_link_peer local=%u peer=%u", local_id, peer_id);
   }
   else if (gb_link_initialized && gb_link.peer_id == peer_id)
   {
      gb_link_set_connected(&gb_link, 0);
      gb_link_initialized = 0;
      fe_evt("gb_link_peer_down peer=%u", peer_id);
   }
}

/* The core enters this callback at the SC write that starts a transfer,
 * before any of the eight bit times elapse (gbcore.h).  Waiting here pauses
 * this endpoint at a byte boundary, never in the middle of a bit;
 * fe_np_pump() drains PSP ad-hoc RX on this emulation thread until the
 * paired byte arrives or the bounded wait expires. */
static int gb_serial_transfer(void *userdata, uint8_t outgoing,
                              int internal_clock, uint8_t *received)
{
   uint8_t peer_id, local_id;
   uint64_t start_us;
   uint16_t transfer_id;
   (void)userdata;
   if (!received || !fe_np_gb_peer_ready(&peer_id) ||
       fe_np_gb_local_id(&local_id) == 0 || !gb_link_initialized ||
       gb_link.peer_id != peer_id || gb_link.local_id != local_id)
      return 0;

   gb_link_completed = 0;
   gb_link_received_byte = 0xFF;
   transfer_id = ++gb_link_next_transfer;
   if (gb_link_start(&gb_link, transfer_id, outgoing,
                     internal_clock ? GB_LINK_INTERNAL_CLOCK :
                                      GB_LINK_EXTERNAL_CLOCK,
                     gb_link_now_ms()) != 0)
   {
      fe_evt("gb_link_start failed tx=%u", outgoing);
      return 0;
   }

   start_us = host.time_us ? host.time_us() : 0;
   while (!gb_link_completed)
   {
      fe_np_pump();
      gb_link_tick(&gb_link, gb_link_now_ms());
      if (gb_link_completed)
         break;
      if (!host.time_us ||
          (host.time_us() - start_us) >= GB_LINK_TIMEOUT_MS * 1000u)
      {
         gb_link_tick(&gb_link, gb_link_now_ms() + GB_LINK_TIMEOUT_MS);
         if (!gb_link_completed)
         {
            /* Abort just this byte.  Leaving the link disconnected here
             * (as before) refused every later byte for the rest of the
             * session while netpacket still reported the peer as up. */
            gb_link_set_connected(&gb_link, 0);
            gb_link_set_connected(&gb_link, 1);
         }
         break;
      }
      if (host.yield_thread)
         host.yield_thread();
   }
   if (!gb_link_completed)
   {
      fe_evt("gb_link_wait failed reason=no_completion");
      return 0;
   }
   *received = gb_link_received_byte;
   fe_evt("gb_link_byte id=%u clock=%s tx=%02x rx=%02x result=%d",
          transfer_id, internal_clock ? "internal" : "external", outgoing,
          *received, (int)gb_link_result);
   /* A timeout/disconnect is completed with the open-bus value. That keeps
    * the game from hanging inside the emulator's CPU loop. */
   return 1;
}

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

/* The table as the first boot found it.  fe_host_option_set_live (FF, the
 * session frameskip policy) edits `options` in place, and the in-process
 * return to the game list loads the next ROM in the same process, so the
 * next game starts from this copy -- the options a fresh boot reads. */
static fe_option options_boot[sizeof(options) / sizeof(options[0])];
static int options_boot_saved;

static unsigned frame_count;
/* Core's audio output rate, learned from retro_get_system_av_info at boot
 * (ADR-0028). 0 until fe_host_boot succeeds. */
static unsigned core_sample_rate;

/* Rendered-vs-emulated accounting (ADR-0019).  The core signals a skipped
 * VIDEO frame by calling video_refresh with data==NULL, so these two
 * counters are the only honest way to tell "the console is behind" from
 * "the console is fine but we threw pictures away".  The field's first
 * session looked healthy on `EVT heartbeat` alone (58.9 fps sustained)
 * while frameskip was quietly halving what the user actually saw. */
static unsigned fps_last_frames, fps_last_rendered, fps_last_skipped;
static uint64_t fps_last_us;

/* Set when an option changed after boot; the core re-reads its options on
 * the next retro_run via GET_VARIABLE_UPDATE (runtime-changeable keys
 * only, e.g. gpsp_frameskip for the FF feature — plan §4.4). */
static int options_dirty;

/* Per-block SRAM dirty map (ADR-0020).  4 KiB matches the GBA flash sector
 * the games erase/write one at a time, so a mid-save dirty check normally
 * touches 1-2 blocks instead of rewriting all 128 KiB to the memory stick. */
#define SRAM_BLOCK   4096
#define SRAM_BLOCKS  (FE_SRAM_SIZE / SRAM_BLOCK)
static uint32_t sram_blk_crc[SRAM_BLOCKS];
static int      sram_have_blk;   /* the .sav on disk matches sram_blk_crc */

/* ---- FF durability nudge ---------------------------------------------------
 * The writer runs BELOW the emulation thread on purpose (emu_prio 0x2B / io
 * 0x2C), and it gets the CPU in the slack main donates at the vblank wait.
 * Unlimited fast-forward never waits, so an in-game save made there could sit
 * in RAM until fast-forward ended -- on PPSSPP indefinitely (the same
 * starvation silenced the event log), on hardware for as long as the GE syncs
 * happen not to leave a gap.  Forced flushes (exit, HOME, quit) always
 * waited; the exposure was a power cut or hard freeze in that window.
 *
 * The signal is the CORE's battery-save write generation (gba_memory.c
 * `backup_write_gen`, bumped only when the game mutates backup memory), so
 * this costs nothing -- not a load of the SRAM, not a CRC -- until a game has
 * actually saved.  Weak: a core without the counter links and never nudges.
 *
 * The writer records the generation it read BEFORE each scan and publishes it
 * as `sram_disk_gen` only once that scan's writes have all landed.  A write
 * that arrives mid-scan or mid-drain moves the live generation past the one
 * published, so it can never be marked persisted by a pass that did not see
 * it.  (A counter, not a bool, for exactly that reason.)
 *
 * The emulation thread, only while fast-forward is on and only once the save
 * has been quiet for SRAM_FF_SETTLE_FRAMES, asks for a scan and sleeps ~1 ms
 * (host_io->yield) every SRAM_FF_NUDGE_EVERY frames until the writer has
 * published the live generation.
 *
 * AT NORMAL SPEED TOO (3.5.x): once the save has been quiet for the same
 * SRAM_FF_SETTLE_FRAMES the emulation thread asks for a scan -- a flag and a
 * wake, no yield: at 1x the writer runs in the slack the vblank wait already
 * gives it.  Up to 3.5.0 a save waited for the writer's own 5 s scan, and a
 * scan that landed in the middle of the game's save left the rest for the
 * NEXT one: for up to five seconds after "saved the game" the stick could
 * hold a newest slot of mixed old and new sectors, which a generation-3
 * Pokemon game reports as "The save file is corrupted" if the console is
 * switched off, crashes or runs flat in that window (reproduced; reported
 * for Sapphire).  The window is now the settle time plus one write.  The
 * 5 s cadence stays as the backstop. */
extern volatile uint32_t backup_write_gen __attribute__((weak));

/* Frames of save silence before the first nudge: a gen-3 Pokemon save
 * writes sectors for ~1 s of game time, and scanning mid-save would only
 * write blocks the game is about to rewrite. */
#define SRAM_FF_SETTLE_FRAMES     30
/* A game that never stops writing still gets persisted: nudge anyway once the
 * oldest unpersisted write is this old. */
#define SRAM_FF_MAX_DEFER_FRAMES  300
/* One ~1 ms yield per this many emulated frames while a save is pending. */
#define SRAM_FF_NUDGE_EVERY       2
/* Give up on one generation after this many yields (~0.3 s of sleeping):
 * a writer that cannot persist (dead stick, no save path) must not tax
 * fast-forward forever.  A new save re-arms it. */
#define SRAM_FF_NUDGE_CAP         300

static volatile uint32_t sram_disk_gen;  /* WRITER: gen whose bytes are on disk */
static volatile uint32_t sram_scan_gen;  /* WRITER: gen read before its last scan */
static volatile int sram_scan_gen_valid; /* WRITER: sram_scan_gen is meaningful */

static int      ffn_enabled = 1;         /* harness A/B knob (sram_ff_nudge) */
static uint32_t ffn_seen_gen;            /* EMU: last gen observed */
static int      ffn_pending;             /* EMU: seen gen not yet on disk */
static unsigned ffn_first_frame;         /* EMU: oldest unpersisted write */
static uint64_t ffn_first_us;            /* EMU: host clock, no later than it */
static unsigned ffn_last_frame;          /* EMU: newest write */
static uint64_t ffn_last_us;             /* EMU: host clock at newest write */
static unsigned ffn_next_frame;          /* EMU: earliest next nudge */
static unsigned ffn_gen_yields;          /* EMU: yields spent on this gen */
static int      ffn_gave_up;             /* EMU: cap hit for this gen */
static unsigned ffn_yields;              /* EMU: lifetime yields */
static int      ffn_starved_said;
/* Wall time after the newest save write by which it must be on the stick
 * at normal speed (the settle time is 0.5 s, a flush tens of ms). */
#define SRAM_STARVED_US 1500000u
/* ...and after the OLDEST unpersisted one (review S2): a game below full
 * speed that writes again at least every SRAM_STARVED_US never trips the
 * rule above, and its writer never gets the CPU.  Longer than
 * SRAM_FF_MAX_DEFER_FRAMES at full speed (5.02 s) plus a healthy flush, so
 * a game that never stops writing but DOES reach full speed is served by
 * the deferred request alone and never sleeps for this. */
#define SRAM_STARVED_OLDEST_US 6500000u
/* One REQUEST = the first ask after the gate opened, until the writer has
 * published a generation at least as new (review S3).  Everything the game
 * wrote up to the ask is then on the stick; whatever it wrote since is newer
 * than the ask, so the "oldest unpersisted write" (frame and clock) moves up
 * to the ask and the deferral starts again from there.  Without this a game
 * that never stops writing was scanned and written every settle period
 * (every other frame in fast-forward) for ever once it had been pending for
 * SRAM_FF_MAX_DEFER_FRAMES; 3.5.0 scanned every 5 s, and so does this.
 * The generation only ever counts up, so the signed difference orders it. */
static int      ffn_asked;               /* EMU: a request is outstanding */
static uint32_t ffn_ask_gen;             /* EMU: live gen when it was made */
static unsigned ffn_ask_frame;
static uint64_t ffn_ask_us;
static uint64_t ffn_yield_us;            /* EMU: lifetime time asleep */

static uint32_t sram_gen_now(void)
{
   return &backup_write_gen ? backup_write_gen : 0u;
}

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

#ifdef REWIND
/* Rewind (fe_rewind.h): what the core callbacks below need to know.  The
 * rest of the glue is above fe_host_run_frame. */
#define RW_AUD_CAP 1200           /* stereo frames per emulated frame, max */
static int       rw_on;           /* fe_host_rewind_enable succeeded */
static int       rw_gesture;      /* a rewind is on screen or winding up: a
                                   * tick has answered something other than
                                   * PLAY and none has answered PLAY since */
static int       rw_replay;       /* this retro_run is a rewind re-run: its
                                   * picture and sound go to the capture (or
                                   * nowhere), never to the platform */
static int       rw_tee;          /* a normal frame: presented AND captured */
static uint16_t *rw_cap_pix;
static const uint16_t *rw_cap_last;
static int16_t  *rw_cap_aud;
static uint32_t  rw_cap_aud_n;
static uint32_t  rw_mask;         /* the input latched for this run */
static int       rw_mask_on;
static FILE     *rw_hash_fp;      /* test instrument (rewind_check.py) */
static int16_t   rw_test_aud[RW_AUD_CAP * 2];
static uint32_t  rw_test_aud_n;
static int       rw_poked;        /* the platform wrote guest RAM itself */
static uint32_t  rw_pitch = FE_GBA_WIDTH;   /* pixels per row of a slot */
static int       rw_cap_got;      /* this frame's slot received a picture */

/* The core's picture (or, on a frame the core skipped, the last one) into
 * the slot at rw_cap_pix. */
static void rw_put_picture(const void *data, size_t pitch)
{
   unsigned y;
   if (data)
      for (y = 0; y < FE_GBA_HEIGHT; y++)
         memcpy(rw_cap_pix + y * rw_pitch,
                (const uint8_t *)data + y * pitch, FE_GBA_WIDTH * 2);
   else if (rw_cap_last && rw_cap_last != rw_cap_pix)
      for (y = 0; y < FE_GBA_HEIGHT; y++)
         memcpy(rw_cap_pix + y * rw_pitch, rw_cap_last + y * rw_pitch,
                FE_GBA_WIDTH * 2);
   else if (!rw_cap_last)
      return;
   rw_cap_got = 1;
}
static void      fe_host_rewind_state_loaded(void);
extern void      gpsp_rewind_mark_all(void);
#endif

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
#ifdef REWIND
   if (rw_replay)
   {
      if (rw_cap_pix)
         rw_put_picture(data, pitch);
      return;
   }
   /* Normal play keeps the pictures of the current window, so that the
    * first tick of a rewind has its frame already. */
   if (rw_tee && rw_cap_pix)
      rw_put_picture(data, pitch);
#endif
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
#ifdef REWIND
   if (rw_replay)
   {
      if (rw_cap_aud && rw_cap_aud_n < RW_AUD_CAP)
      {
         size_t n = RW_AUD_CAP - rw_cap_aud_n;
         if (n > frames)
            n = frames;
         memcpy(rw_cap_aud + rw_cap_aud_n * 2, data, n * 4);
      }
      rw_cap_aud_n += (uint32_t)frames;   /* past the cap = clipped, counted */
      return frames;
   }
   if (rw_tee)
   {
      if (rw_cap_aud_n < RW_AUD_CAP)
      {
         size_t n = RW_AUD_CAP - rw_cap_aud_n;
         if (n > frames)
            n = frames;
         memcpy(rw_cap_aud + rw_cap_aud_n * 2, data, n * 4);
      }
      rw_cap_aud_n += (uint32_t)frames;
   }
   if (rw_hash_fp && rw_test_aud_n < RW_AUD_CAP)
   {
      size_t n = RW_AUD_CAP - rw_test_aud_n;
      if (n > frames)
         n = frames;
      memcpy(rw_test_aud + rw_test_aud_n * 2, data, n * 4);
      rw_test_aud_n += (uint32_t)n;
   }
#endif
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
#ifdef REWIND
   /* One value per frame, the one that is logged: a replay must hand the
    * core exactly what the first run did. */
   if (rw_mask_on)
      mask = rw_mask;
   else
#endif
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
   if (gb_family && !gb_core)
   {
      /* A link session: the local machine's cartridge RAM (autopilot
       * waitsram).  Nothing of it is ever flushed from here. */
      gbdual_t *d = gbl ? fe_gblink_dual(gbl) : NULL;
      int slot = fe_gblink_local_slot(gbl);
      const gbcore_api_t *api = gbdual_api(slot);
      size_t n;
      if (!d || !api)
         return 0;
      n = api->cart_ram_size(gbdual_core(d, slot));
      if (!n || gb_save_reserve(n + GB_SAVE_TRAILER_MAX) != 0 ||
          api->save_ram_read(gbdual_core(d, slot), gb_save_buffer,
                             gb_save_capacity) != 0)
         return 0;
      return fe_crc32(0, gb_save_buffer, n);
   }
   if (gb_core)
   {
      /* Same RAM-only span as the flush's change detection: the clock
       * trailer would make this CRC tick every second on RTC carts. */
      size_t size = gbcore_save_ram_size(gb_core);
      if (!size || gb_save_reserve(size) != 0 ||
          gbcore_save_ram_read(gb_core, gb_save_buffer, gb_save_capacity) != 0)
         return 0;
      return gb_save_ram_crc(size);
   }
   const void *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   return p ? fe_crc32(0, p, FE_SRAM_SIZE) : 0;
}

/* A "Share game data" pull replaces the save with two renames
 * (fe_share_commit): save -> save.bak, then the new file -> save.  Power
 * lost between them leaves NO save and the old one as save.bak; the game
 * would boot empty, the player would save, and the next pull would delete
 * the only copy of the original.  So before a save is loaded: no file, but
 * a .bak beside it -> the .bak is the save (review finding 5). */
static int save_recover_bak(const char *path)
{
   char bak[512];
   int exists;
   if (!path || snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
      return -1;
   /* A pending whole-image transaction is newer than a retained share
    * backup. Resolve it before considering .bak, and never overwrite the
    * sole old image when recovery is blocked. */
   if (fe_host_state_recover(path) < 0)
      return -1;
   exists = state_file_exists(path);
   if (exists != 0)
      return exists < 0 ? -1 : 0;
   exists = state_file_exists(bak);
   if (exists <= 0)
      return exists;
   if (state_file_rename(bak, path) != 0)
   {
      fe_log("save_recover: could not restore %s", bak);
      return -1;
   }
   fe_evt("save_recovered from=.bak (an interrupted share replace)");
   return 1;
}

static int sram_load(void)
{
   void *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   FILE *f;
   size_t n = 0;
   int exact = 0, read_ok;

   battery_load_failed = 0;
   if (!p || !host.save_path)
      return 0;
   battery_load_failed = 1;
   sram_have_blk = 0;
   if (save_recover_bak(host.save_path) < 0)
      goto failed;

   f = fopen(host.save_path, "rb");
   if (f)
   {
      n = fread(p, 1, FE_SRAM_SIZE, f);
      exact = (n == FE_SRAM_SIZE) && (fgetc(f) == EOF);   /* no trailing junk */
      read_ok = !ferror(f);
      if (fclose(f) != 0)
         read_ok = 0;
      if (!read_ok)
         goto failed;
      fe_evt("sram_load size=%u crc=%08x", (unsigned)n, (unsigned)sram_crc());
   }
   else
   {
      if (errno != ENOENT)
         goto failed;
      fe_evt("sram_load size=0 crc=none");
   }

   /* Whatever the game has written so far is what was just loaded. */
   sram_disk_gen = ffn_seen_gen = sram_gen_now();
   sram_scan_gen_valid = 0;
   ffn_pending = 0;
   ffn_asked = 0;
   ffn_gave_up = 0;
   ffn_gen_yields = 0;

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
   battery_load_failed = 0;
   return 0;
failed:
   fe_log("sram_load FAILED; preserving battery file %s", host.save_path);
   return -1;
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
   if (gb_family)
   {
      /* GB addresses (0000-FFFF) for GB scripts: the single machine, or in
       * a link session this console's own machine. */
      gbdual_t *d = gbl ? fe_gblink_dual(gbl) : NULL;
      if (gba_addr > 0xFFFF)
         return -1;
      if (gb_core)
         return gbcore_peek(gb_core, (uint16_t)gba_addr, out, len);
      if (!d)
         return -1;
      return gbdual_api(fe_gblink_local_slot(gbl))->peek(
         gbdual_core(d, fe_gblink_local_slot(gbl)), (uint16_t)gba_addr, out,
         len);
   }

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
   if (gb_family)
      return -1;

   if (gba_addr >= 0x08000000)
      return -1;
#ifdef REWIND
   /* The platform writing guest RAM is a writer the store paths never see
    * (gba_memory.h, "THE RULE"), and one no input log can replay: every
    * page is marked and the history ends at this frame. */
   if (rw_on)
   {
      gpsp_rewind_mark_all();
      rw_poked = 1;
   }
#endif
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

/* Save states are whole snapshots: updating one in place cannot recover a
 * partial write. Keep the old slot until its replacement has been closed.
 * .old is also the interrupted-share backup (fe_share_commit). */
static int state_file_exists(const char *path)
{
   struct stat st;
   if (stat(path, &st) == 0)
      return 1;
   return errno == ENOENT ? 0 : -1;
}

static int state_file_rename(const char *from, const char *to)
{
   (void)rename(from, to);
   /* PSP and PPSSPP can disagree about rename's return value. Verify both
    * names, as the PSP share implementation does. */
   return state_file_exists(from) == 0 && state_file_exists(to) == 1 ? 0 : -1;
}

int fe_host_state_recover(const char *path)
{
   char old[512];
   int exists;
   if (!path || snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old))
      return -1;
   exists = state_file_exists(path);
   if (exists != 0)
      return exists < 0 ? -1 : 0;
   exists = state_file_exists(old);
   if (exists <= 0)
      return exists;
   if (state_file_rename(old, path) != 0)
      return -1;
   fe_evt("state_recovered file=%s from=.old", path);
   return 1;
}

/* A writer may stream a stable snapshot in small blocks; it returns zero
 * only when all bytes reached stdio. This helper owns close and promotion. */
static int state_file_replace_writer(const char *path,
                                     int (*write_image)(FILE *, void *),
                                     void *user)
{
   char tmp[512], old[512];
   FILE *f;
   int ok, exists;
   if (!path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
       snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old) ||
       fe_host_state_recover(path) < 0)
      return -1;
   f = fopen(tmp, "wb");
   if (!f)
      return -1;
   ok = write_image(f, user) == 0;
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
   {
      remove(tmp);
      return -1;
   }
   exists = state_file_exists(path);
   if (exists < 0)
   {
      remove(tmp);
      return -1;
   }
   if (exists)
   {
      /* Never remove the current slot to make room. A failed rename or
       * backup cleanup leaves it in place. */
      if ((remove(old) != 0 && errno != ENOENT) ||
          state_file_rename(path, old) != 0)
      {
         remove(tmp);
         return -1;
      }
   }
   if (state_file_rename(tmp, path) != 0)
   {
      if (exists)
         (void)state_file_rename(old, path);
      remove(tmp);
      /* If rollback also failed, .old stays recoverable on the next load
       * or slot scan. Never overwrite it with an empty/new slot. */
      return -1;
   }
   if (exists)
      remove(old);
   return 0;
}

typedef struct
{
   const void *data;
   size_t size;
} state_file_image;

static int state_file_write_image(FILE *f, void *user)
{
   const state_file_image *image = (const state_file_image *)user;
   return fwrite(image->data, 1, image->size, f) == image->size ? 0 : -1;
}

static int state_file_replace(const char *path, const void *data, size_t size)
{
   state_file_image image = { data, size };
   return state_file_replace_writer(path, state_file_write_image, &image);
}

int fe_host_state_save(const char *path)
{
   size_t sz;

   if (gb_family && !gb_core)
   {
      fe_evt("state_save file=%s FAILED reason=link_session", path);
      return -1;
   }
#ifdef REWIND
   /* While a rewind is on screen the machine is NOT at the frame shown (it
    * is re-running older ones): a state saved now would be of the wrong
    * moment.  The platform lands it first (fe_host_rewind_land); this is
    * the refusal for one that did not.  (rw_gesture, not
    * fe_rewind_active(): that stays true after a landing until the first
    * frame is played, and refused the save the landing was made for.) */
   if (rw_on && rw_gesture)
   {
      fe_evt("state_save file=%s FAILED reason=rewinding", path);
      return -1;
   }
#endif
   if (gb_core)
   {
      /* The same staging buffer: the largest GB image (CGB with 128 KiB of
       * cartridge RAM) is under half of it. */
      long n = gbcore_state_save(gb_core, state_buf, sizeof(state_buf));
      if (n <= 0)
      {
         fe_evt("state_save file=%s FAILED reason=serialize sz=%u", path,
                (unsigned)gbcore_state_size(gb_core));
         return -1;
      }
      sz = (size_t)n;
   }
   else
   {
      sz = retro_serialize_size();
      if (sz == 0 || sz > sizeof(state_buf) || !retro_serialize(state_buf, sz))
      {
         fe_evt("state_save file=%s FAILED reason=serialize sz=%u", path,
                (unsigned)sz);
         return -1;
      }
   }
   if (state_file_replace(path, state_buf, sz) != 0)
   {
      fe_evt("state_save file=%s FAILED reason=io", path);
      return -1;
   }
   fe_evt("state_save file=%s size=%u crc=%08x", path, (unsigned)sz,
          (unsigned)fe_crc32(0, state_buf, sz));
   return 0;
}

int fe_host_state_load(const char *path)
{
   size_t sz;
   size_t n;
   int whole, read_ok;
   FILE *f;

   if (fe_host_state_recover(path) < 0)
      return -1;
   f = fopen(path, "rb");

   if (!f)
   {
      fe_evt("state_load file=%s FAILED reason=open", path);
      return -1;
   }
   n = fread(state_buf, 1, sizeof(state_buf), f);
   whole = fgetc(f) == EOF;
   read_ok = !ferror(f);
   if (fclose(f) != 0)
      read_ok = 0;
   if (!whole || !read_ok)
   {
      fe_evt("state_load file=%s FAILED reason=io_or_size", path);
      return -1;
   }
   if (gb_family && !gb_core)
   {
      fe_evt("state_load file=%s FAILED reason=link_session", path);
      return -1;
   }
   if (gb_core)
   {
      /* The core validates the image (magic, version, console, cartridge
       * identity, exact size) before touching any machine state. */
      if (gbcore_state_load(gb_core, state_buf, n) != 0)
      {
         fe_evt("state_load file=%s FAILED reason=unserialize n=%u", path,
                (unsigned)n);
         return -1;
      }
      fe_evt("state_load file=%s size=%u", path, (unsigned)n);
      return 0;
   }
   sz = retro_serialize_size();
   if (n != sz || !retro_unserialize(state_buf, sz))
   {
      fe_evt("state_load file=%s FAILED reason=unserialize n=%u", path,
             (unsigned)n);
      return -1;
   }
   fe_evt("state_load file=%s size=%u", path, (unsigned)sz);
#ifdef REWIND
   if (rw_on)                    /* a loaded state starts a new history */
      fe_host_rewind_state_loaded();
#endif
#ifdef GBA_LINK
   if (host.gba_dual)
      fe_gbadual_state_loaded(state_buf, sz);
#endif
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
 * write).  Once async ownership has ended, synchronous shutdown recovery may
 * convert an orphaned WRITING block back to DIRTY for retry.  So a block the
 * game touches mid-write is re-marked DIRTY, the writer's compare-and-clear
 * fails, and the next pass rewrites it.  That is the whole synchronisation,
 * and it needs no lock. */
#define SBLK_CLEAN    0
#define SBLK_DIRTY    1
#define SBLK_WRITING  2

static FILE    *sram_fp;            /* held open "r+b"; WRITER-owned */
static volatile unsigned char sram_blk_state[SRAM_BLOCKS];
static volatile int sram_full_req;  /* whole-image rewrite pending */
/* A delta write failed and its dirty set was discarded (sram_abandon_deltas):
 * the image in memory is NOT on disk and nothing is queued to put it there.
 * Cleared by the whole-image rewrite that makes the disk whole again. */
static volatile int sram_retry_full;

/* Harness only (sleep_sim_sav, sram_exit_retry): PPSSPP never suspends, so
 * the one thing a real sleep does to the save path -- kill the handle the
 * .sav is held open on -- has to be modelled.  `sram_fp_dead` makes every
 * write through the CURRENT handle fail as an I/O error until it is closed;
 * a fresh open works, as on a console.  `sram_exit_retry = 0` restores
 * 3.5.0's exit, which gave up after that failure, for the A/B. */
#ifdef GPSP_PERF_RIG
static int sram_fp_dead;
static int sram_exit_retry = 1;
#define SRAM_FP_DEAD     sram_fp_dead
#define SRAM_EXIT_RETRY  sram_exit_retry
#else
#define SRAM_FP_DEAD     0
#define SRAM_EXIT_RETRY  1
#endif
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

/* The async writer can be terminated at shutdown if a memory-stick call does
 * not return within the join timeout.  In that case it may leave one block in
 * WRITING, a state only that writer normally owns.  Once host_io is cleared,
 * no writer can still be using the block or sram_fp, so make that interrupted
 * block retryable.  Do not do this while async ownership is installed: a live
 * writer may still be copying or writing the block, and its normal
 * compare-and-clear preserves any concurrent DIRTY transition. */
static void sram_recover_interrupted_writes(void)
{
   unsigned i;
   for (i = 0; i < SRAM_BLOCKS; i++)
      if (sram_blk_state[i] == SBLK_WRITING)
         sram_blk_state[i] = SBLK_DIRTY;
}

static void sram_close(void)
{
   if (sram_fp)
   {
      fclose(sram_fp);
      sram_fp = NULL;
#ifdef GPSP_PERF_RIG
      sram_fp_dead = 0;
#endif
   }
}

#ifdef GPSP_PERF_RIG
int fe_host_sram_test_kill_handle(void)
{
   if (!sram_fp)
      return 0;
   sram_fp_dead = 1;
   return 1;
}

void fe_host_sram_test_exit_retry(int on)
{
   sram_exit_retry = on ? 1 : 0;
}
#endif

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
   sram_retry_full = 1;
}

typedef struct
{
   const uint8_t *live;
   uint32_t crc[SRAM_BLOCKS];
} sram_full_image;

static int sram_write_full_blocks(FILE *f, void *user)
{
   sram_full_image *image = (sram_full_image *)user;
   unsigned i;
   for (i = 0; i < SRAM_BLOCKS; i++)
   {
      /* The game may change live RAM while I/O yields to its thread. Record
       * the bytes this write owns, never a later view of the live buffer. */
      memcpy(sram_stage, image->live + (size_t)i * SRAM_BLOCK, SRAM_BLOCK);
      image->crc[i] = fe_crc32(0, sram_stage, SRAM_BLOCK);
      if (fwrite(sram_stage, 1, SRAM_BLOCK, f) != SRAM_BLOCK)
         return -1;
   }
   return 0;
}

/* Whole rewrites use the same replacement transaction as save states.
 * Keep the existing 4 KiB stage and only 128 bytes of CRCs on the stack;
 * the PSP cannot afford another 128 KiB battery snapshot allocation. */
static int sram_write_full(const void *p)
{
   sram_full_image image;
   if (battery_load_failed)
      return -1;
   image.live = (const uint8_t *)p;
   sram_close();
   if (save_recover_bak(host.save_path) < 0 ||
       state_file_replace_writer(host.save_path, sram_write_full_blocks,
                                  &image) != 0)
   {
      fe_log("sram_flush whole replacement FAILED on %s", host.save_path);
      return -1;
   }
   memcpy(sram_blk_crc, image.crc, sizeof(image.crc));
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

   if (battery_load_failed)
      return -1;
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
         sram_blk_state[i] = SBLK_CLEAN;
      }
      sram_run_bytes += FE_SRAM_SIZE;
      sram_run_blocks = SRAM_BLOCKS;
      sram_run_full = 1;
      sram_have_blk = 1;
      sram_full_req = 0;
      sram_retry_full = 0;
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
         if (SRAM_FP_DEAD ||
             fseek(sram_fp, (long)i * SRAM_BLOCK, SEEK_SET) != 0 ||
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
      /* The save was in flight for longer than half a second: say so, and
       * which kind.  A slow single write is the stick; fast writes and a
       * long total is the writer thread not getting the CPU (main never
       * blocked: fast-forward, a game below full speed, a rewind), which
       * the nudges below exist to prevent.  Must not appear. */
      if (sram_run_us > 500000u || sram_run_scan_us > 500000u)
         fe_evt("sram_flush_slow ms=%u scan_ms=%u blk_ms=%u why=%s",
                (unsigned)(sram_run_us / 1000),
                (unsigned)(sram_run_scan_us / 1000),
                (unsigned)(sram_run_blk_max / 1000),
                sram_run_blk_max > 250000u ? "stick" : "writer_starved");
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
static volatile int sram_io_busy;
static int sram_service_io_body(void)
{
   uint64_t now = host.time_us ? host.time_us() : 0;
   unsigned before;
   int req = sram_scan_req;

   if (battery_load_failed)
      return 0;
   if (gb_family)
   {
      /* The GB core owns its cartridge RAM on the emulation thread, so the
       * snapshot is taken there (gb_save_flush_periodic); this thread only
       * writes the copy it was handed. */
      return gb_save_service_io();
   }

   /* The scan is on this thread too since ADR-0026: it is 11 ms of CPU on
    * the user's hardware, and on a single-core console the only place to
    * put 11 ms that does not cost the emulator a frame is the slack it
    * already donates at the vblank wait. */
   if (req || (sram_last_scan_us &&
               now - sram_last_scan_us >= SRAM_SCAN_INTERVAL_US))
   {
      /* Read the generation BEFORE the scan: anything the game writes from
       * here on may or may not be in what this pass sees, so it must not be
       * credited to it (FF durability nudge). */
      uint32_t gen = sram_gen_now();
      sram_last_scan_us = now;
      sram_run_threaded = 1;
      {
         /* A scan made while the save is HELD (a link session owns it)
          * returns without looking: it has seen nothing, so it must not
          * become the generation that is later published as on disk
          * (review S5).  Whoever releases the hold asks for a scan. */
#ifdef GBA_LINK
         int held = gba_sram_hold;
#else
         int held = 0;
#endif
         (void)sram_scan(req == 2);
         sram_scan_req = 0;      /* clear only after the scan has marked */
#ifdef GBA_LINK
         held |= gba_sram_hold;
#endif
         if (!held)
         {
            sram_scan_gen = gen;
            sram_scan_gen_valid = 1;
         }
      }
   }

   before = sram_pending();
   if (sram_full_req || before)
   {
      sram_run_threaded = 1;
      if (sram_drain(0) != 0)
         return 0;            /* nothing is persisted on an I/O error */
   }
   /* Everything the last scan asked for is on disk, and the on-disk image is
    * a trustworthy baseline (a failed write clears sram_have_blk until a
    * full rewrite succeeds): publish the generation that scan saw. */
   if (sram_scan_gen_valid && sram_have_blk && !sram_full_req &&
       !sram_pending())
      sram_disk_gen = sram_scan_gen;
   { unsigned after = sram_pending();
     return (int)(before > after ? before - after : 0); }
}

int fe_host_sram_service_io(void)
{
   int rc;
   sram_io_busy = 1;
   rc = sram_service_io_body();
   sram_io_busy = 0;
   return rc;
}

/* The console is about to sleep (PSP power callback, its own thread): ask
 * the writer for a scan now, so a save made in the last few seconds is on
 * the stick before the power goes.  A flag and a wake -- no file is touched
 * from the caller's thread.  Nothing to do without a writer thread (the
 * synchronous path flushes on the emulation thread) or for a GB game (its
 * save is flushed by the main loop's own suspend request). */
void fe_host_sram_request_scan(void)
{
   /* One read: this runs on the power callback's thread, and the main
    * thread clears host_io when it stops the writer (io_thread_stop). */
   const fe_host_io *io = host_io;
   if (!io || gb_family || !host.save_path)
      return;
   if (!sram_scan_req)
      sram_scan_req = 1;
   io->wake();
}

void fe_host_sram_ff_nudge_enable(int on)
{
   ffn_enabled = on ? 1 : 0;
}

int fe_host_sram_ff_nudge(int ff_active)
{
   uint32_t gen, dgen;
   uint64_t now_us;
   int oldest_late, want_scan;

   if (!host_io || gb_family || !&backup_write_gen || !host.save_path)
      return 0;

   gen = backup_write_gen;
   if (gen != ffn_seen_gen)
   {
      now_us = host.time_us ? host.time_us() : 0;
      if (!ffn_pending)
      {
         ffn_first_frame = frame_count;
         ffn_first_us = now_us;
         ffn_asked = 0;
      }
      /* A new SAVE re-arms the yield budget: the first write, or one that
       * follows a settle period of quiet.  A write in the middle of a run
       * of them does not, or a game that never stops writing would be made
       * to sleep for a writer that cannot persist for as long as it ran. */
      if (!ffn_pending ||
          frame_count - ffn_last_frame >= SRAM_FF_SETTLE_FRAMES)
      {
         ffn_gen_yields = 0;
         ffn_gave_up = 0;
         ffn_starved_said = 0;
      }
      ffn_pending = 1;
      ffn_seen_gen = gen;
      ffn_last_frame = frame_count;
      ffn_last_us = now_us;
   }
   if (!ffn_pending)
      return 0;
   dgen = sram_disk_gen;
   now_us = host.time_us ? host.time_us() : 0;
   if (dgen == gen)
   {
      /* Persisted -- by a nudged scan or by the writer's own cadence.
       * Observed here, so both figures are upper bounds by one frame. */
      fe_evt("sram_persisted gen=%u ff=%d frames_since_write=%u "
             "ms_since_write=%u yields=%u total_yields=%u total_yield_us=%u",
             (unsigned)gen, ff_active, frame_count - ffn_last_frame,
             (unsigned)((now_us - ffn_last_us) / 1000u), ffn_gen_yields,
             ffn_yields, (unsigned)ffn_yield_us);
      ffn_pending = 0;
      ffn_asked = 0;
      ffn_gen_yields = 0;
      return 0;
   }
   if (ffn_asked && (int32_t)(dgen - ffn_ask_gen) >= 0)
   {
      /* The request was served, and the game has written again since: what
       * is still unpersisted is newer than the request (see ffn_asked). */
      ffn_asked = 0;
      ffn_first_frame = ffn_ask_frame;
      ffn_first_us = ffn_ask_us;
      ffn_gen_yields = 0;
      ffn_gave_up = 0;
      ffn_starved_said = 0;
   }
   if (!ffn_enabled)
      return 0;
#ifdef GBA_LINK
   /* A link session owns the battery image and the writer looks at nothing
    * (gba_sram_hold): no request can be served, so none is made and main
    * does not sleep for one.  The release asks for the scan itself. */
   if (gba_sram_hold)
      return 0;
#endif
   oldest_late = host.time_us &&
                 now_us - ffn_first_us > SRAM_STARVED_OLDEST_US;
   if (frame_count - ffn_last_frame < SRAM_FF_SETTLE_FRAMES &&
       frame_count - ffn_first_frame < SRAM_FF_MAX_DEFER_FRAMES &&
       !oldest_late)
      return 0;
   if ((int)(frame_count - ffn_next_frame) < 0)
      return 0;
   /* Fast-forward is not the only time main never blocks: a game that does
    * not reach full speed gives the writer (which runs below main) no time
    * either.  A save still not on the stick a second after its settle time,
    * or whose OLDEST write has waited SRAM_STARVED_OLDEST_US while the game
    * kept writing, is treated as fast-forward treats it: main sleeps for it.
    * Once the yield cap for this save is spent (a writer that cannot
    * persist) it is back to asking every settle period, as normal speed
    * always did. */
   if (!ff_active && !ffn_gave_up && host.time_us &&
       (oldest_late || now_us - ffn_last_us > SRAM_STARVED_US))
   {
      if (!ffn_starved_said)
      {
         ffn_starved_said = 1;
         fe_evt("sram_nudge_starved gen=%u disk_gen=%u ms_since_write=%u "
                "ms_since_oldest=%u",
                (unsigned)gen, (unsigned)dgen,
                (unsigned)((now_us - ffn_last_us) / 1000u),
                (unsigned)((now_us - ffn_first_us) / 1000u));
      }
      ff_active = 1;
   }
   if (ff_active)
   {
      if (ffn_gave_up)
         return 0;
      if (ffn_gen_yields >= SRAM_FF_NUDGE_CAP)
      {
         ffn_gave_up = 1;
         fe_evt("sram_ff_nudge gave_up gen=%u disk_gen=%u yields=%u",
                (unsigned)gen, (unsigned)dgen, ffn_gen_yields);
         return 0;
      }
   }
   if (!ffn_asked)
   {
      ffn_asked = 1;
      ffn_ask_gen = gen;
      ffn_ask_frame = frame_count;
      ffn_ask_us = now_us;
   }
   /* The writer has not scanned since the request, or its last pass failed
    * (no trustworthy on-disk image until a full rewrite, which only a scan
    * schedules): ask for a scan now rather than at its 5 s cadence -- once
    * per request, not once per nudge.  Never downgrade a pending forced
    * request. */
   want_scan = !sram_scan_gen_valid || !sram_have_blk ||
               (int32_t)(sram_scan_gen - ffn_ask_gen) < 0;
   if (want_scan && !sram_scan_req)
      sram_scan_req = 1;
   if (!ff_active)
   {
      /* Normal speed: ask, do not wait.  The writer is woken again every
       * settle period until the generation is on disk (a scan that raced a
       * late write, a write that failed and needs the whole-image
       * rewrite). */
      ffn_next_frame = frame_count + SRAM_FF_SETTLE_FRAMES;
      host_io->wake();
      return 0;
   }
   ffn_next_frame = frame_count + SRAM_FF_NUDGE_EVERY;
   ffn_gen_yields++;
   ffn_yields++;
   {
      uint64_t t0 = host.time_us ? host.time_us() : 0;
      host_io->wake();
      host_io->yield();
      if (t0)
         ffn_yield_us += host.time_us() - t0;
   }
   return 1;
}

/* Per-frame service: keep an in-progress flush moving without ever handing
 * the memory stick a whole 128 KiB image in one frame.  Called from
 * fe_host_run_frame; cheap no-op when nothing is pending. */
void fe_host_sram_service(void)
{
   if (gb_family)
   {
      if (!gb_core)
         return;           /* link session: saves commit only at its end */
      if ((frame_count % SRAM_CHECK_INTERVAL) == 0)
         (void)gb_save_flush_periodic();
      return;
   }
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
static int sram_sync_checked(void)
{
   if (battery_load_failed)
      return -1;
   if (gb_family)
   {
      /* Must run on the emulation thread: only it may read the GB core's RAM.
       * Not forced: exit reaches this two or three times (io_thread_stop,
       * then fe_host_shutdown's flush + sync) and an unchanged cartridge
       * needs no memory-stick write at all. */
      return gb_save_flush(0);
   }
   if (!host_io)
   {
      /* Synchronous ownership means the async writer has stopped.  Recover
       * its interrupted block before draining; sram_drain deliberately only
       * takes DIRTY blocks, so leaving WRITING here would strand the save. */
      sram_recover_interrupted_writes();
      if (sram_pending() || sram_full_req)
      {
         if (sram_drain(0) != 0)
            sram_full_req = 1;   /* retry from a whole-image write */
      }
      /* A FAILED DELTA WRITE MUST NOT END THE STORY HERE.
       *
       * sram_abandon_deltas() closes the file, forgets the dirty set and
       * leaves the whole-image rewrite to "the next scan".  This function
       * is what exit calls, and at exit there is no next scan: the flush
       * that failed returned with nothing pending, this returned with
       * nothing to do, and the save stayed in memory.
       *
       * It is reachable on a console without anything being broken: the
       * .sav is held open for the run, and a PSP invalidates open Memory
       * Stick files across sleep, so the first write after a wake fails.
       * Save, sleep, wake, quit within the writer's 5 s scan interval, and
       * the stick kept whatever it had -- the previous save, or, when a
       * scan had landed in the middle of the game's save before the sleep,
       * a slot of mixed old and new sectors ("The save file is corrupted").
       *
       * So finish it now, with the rewrite the next scan would have asked
       * for: a fresh open of the path, the whole image.  One attempt; if
       * the stick really is gone there is nothing more to do from here. */
      if ((sram_retry_full || sram_full_req) && SRAM_EXIT_RETRY)
      {
         sram_full_req = 1;
         (void)sram_drain(0);
      }
      return sram_pending() || sram_full_req || sram_retry_full ? -1 : 0;
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
         if (!sram_io_busy && !sram_scan_req && !sram_pending() && !sram_full_req)
            return sram_retry_full ? -1 : 0;
         host_io->yield();
      }
   }
   fe_evt("sram_sync_timeout blocks=%u full=%d scan=%d", sram_pending(),
          sram_full_req, sram_scan_req);
   return -1;
}

void fe_host_sram_sync(void)
{
   (void)sram_sync_checked();
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
#ifdef GBA_LINK
   if (gba_sram_hold && !gba_sram_quiescing)
      return 0;         /* a GBA link session owns the battery image */
#endif

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
   /* Nothing to do only when every block matches what is ON DISK
    * (sram_blk_crc, recorded from the bytes each write staged).  This used
    * to compare the whole image with the image the previous SCAN saw: a
    * block that changed again between that scan and its write, then went
    * back, left the intermediate bytes on the stick and a scan that called
    * the save persisted (review S4).  A failed delta write invalidates the
    * per-block baseline (sram_have_blk): scan on, to schedule the full
    * rewrite that is the only way to re-establish it. */
   if (!force_write && sram_have_blk)
   {
      for (i = 0; i < SRAM_BLOCKS; i++)
         if (crc_now[i] != sram_blk_crc[i])
            break;
      if (i == SRAM_BLOCKS)
      {
         sram_run_scan_us = 0;    /* nothing to attribute it to */
         return 0;
      }
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

   return 1;
}

int fe_host_sram_flush(int force_write)
{
   if (battery_load_failed)
      return -1;
   if (gb_family)
      return gb_save_flush(force_write);
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

   FE_EVT_ONLY(size);

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

/* ---- GB link session (fe_gblink.h) -----------------------------------
 *
 * The platform brings the ad-hoc session up (fe_np_start_gb), then calls
 * fe_host_gblink_start: the running game is flushed and shut down, and the
 * session negotiates, transfers what it must and powers BOTH players' Game
 * Boys on.  From then fe_host_run_frame steps the session instead of the
 * single machine.  When it has ended (DONE: this console's save written;
 * FAILED: nothing written) the platform calls fe_host_gblink_close, which
 * boots the game again from its save. */

static int gbl_send(void *user, const void *buf, size_t len)
{
   uint8_t id;
   (void)user;
   if (!fe_np_gb_peer_ready(&id))
      return -1;
   return fe_np_gb_send(id, buf, len);
}

/* Cartridge and save chunks go at GBL_BULK_PER_FRAME a frame, with at most
 * GBL_BULK_BACKLOG of netdrv's reliable payloads queued or in flight.
 *
 * The rate (PPSSPP, 139-byte chunks, 8 MiB cart): the receiver's transport
 * ring holds 64 datagrams between two drains, one a frame.  24 a frame ran
 * clean at 200 KB/s, 32 clean at 267 KB/s (retx 2 in 60,000), 40 and 48
 * overran the ring (ringdrop), and each drop cost a retransmission timeout.
 * Unpaced, 2 MiB took 257 s.
 *
 * The backlog: netdrv_send_capacity counts its spill list, ~900 payloads,
 * and filling it turned those drops into a collapse -- at 40 a frame the
 * sender queued 800+ chunks, every loss was resent behind them, the RTT
 * grew to 350 ms and the rate fell to a few chunks a frame.  96 covers the
 * round trip at 32 a frame (~2 frames) with room to spare, and keeps the
 * session's own messages (inputs, hashes) from waiting behind a cartridge. */
#ifndef GBL_BULK_PER_FRAME
#define GBL_BULK_PER_FRAME 32
#endif
#define GBL_BULK_BACKLOG 96
static unsigned gbl_bulk = GBL_BULK_PER_FRAME;
void fe_host_gblink_tune(unsigned bulk_per_frame)
{
   if (bulk_per_frame)
      gbl_bulk = bulk_per_frame;
}

static int gbl_room(void *user)
{
   (void)user;
   return GBL_BULK_BACKLOG - fe_np_gb_unacked();
}

int fe_host_gblink_flushed(void)
{
   return fe_np_gb_unacked() == 0;
}

static int gbl_bulk_send(void *user, const void *buf, size_t len)
{
   uint8_t mac[6];
   (void)user;
   if (!host.gb_bulk_send || fe_np_gb_peer_mac(mac) != 0)
      return -1;
   return host.gb_bulk_send(mac, buf, len);
}

static int gbl_find(void *user, uint32_t size, const uint8_t header[0x1C],
                    char *path, size_t cap)
{
   (void)user;
   return host.gb_find_rom ? host.gb_find_rom(size, header, path, cap) : 0;
}

/* A battery image's clock trailer (VBA-M/BGB layout: 48 bytes, or the
 * 44-byte variant with a 32-bit stamp) re-stamped with THIS PSP's clock:
 * the registers keep the time the game's clock shows, the stamp says "as of
 * now", so the next load advances it by exactly the time that passes from
 * here.  A session's images are stamped with the session clock (the host's,
 * advanced by emulated frames); written back unchanged, a PSP whose clock
 * reads earlier (the PSP-1000 says 2011) loads a stamp from its future, the
 * core calls the clock not real and resets it, and Crystal asks for the time
 * again -- the owner's report after the first hand-played trade. */
static void gb_rtc_to_local(uint8_t *image, size_t size)
{
   /* Cartridge RAM sizes are multiples of 256 bytes (MBC2's 512 included),
    * so a remainder of 48 or 44 is a trailer. */
   size_t ram = size - size % 0x100;
   int64_t now;
   uint8_t *t;
   if (!host.wallclock || size < 0x100 || (size - ram != 48 && size - ram != 44))
      return;
   if (host.gblink_rtc_keep_session_stamp)
   {
      fe_evt("gb_rtc_rebase skipped (harness A/B)");
      return;
   }
   now = (int64_t)host.wallclock();
   if (now <= 0)
      return;
   t = image + ram;
   t[40] = (uint8_t)now; t[41] = (uint8_t)(now >> 8);
   t[42] = (uint8_t)(now >> 16); t[43] = (uint8_t)(now >> 24);
   if (size - ram == 48)
   {
      t[44] = (uint8_t)(now >> 32); t[45] = (uint8_t)(now >> 40);
      t[46] = (uint8_t)(now >> 48); t[47] = (uint8_t)(now >> 56);
   }
   /* Stamped with this frontend's clock: UTC, and marked as the files
    * gb_save_flush writes are. */
   if (size - ram == 48)
      gb_rtc_mark(t);
   fe_evt("gb_rtc_rebase to_local=%lld trailer=%u", (long long)now,
          (unsigned)(size - ram));
}

static int gbl_commit(void *user, const void *image, size_t size)
{
   int ok;
   uint8_t *local;
   (void)user;
   if (!host.save_path || battery_load_failed)
      return -1;
   local = (uint8_t *)malloc(size);
   if (!local)
      return -1;
   memcpy(local, image, size);
   gb_rtc_to_local(local, size);
   image = local;
   ok = save_recover_bak(host.save_path) >= 0 &&
        state_file_replace(host.save_path, image, size) == 0;
   gb_save_file_len = ok ? (long)size : -1;
   gb_save_crc_valid = 0;
   fe_evt("gblink_save_commit size=%u crc=%08x ok=%d", (unsigned)size,
          fe_crc32(0, image, size), ok);
   free(local);
   return ok ? 0 : -1;
}

static void gbl_video(void *user, const gbcore_video_frame_t *frame)
{
   gb_skip_frame = 0;
   gb_video_frame(user, frame);
}

static int gb_single_boot(void)
{
   gbcore_callbacks_t cb;
   memset(&cb, 0, sizeof(cb));
   cb.video = gb_video_frame;
   cb.audio_batch = gb_audio_batch;
   cb.serial_transfer = gb_serial_transfer;
   gbcore_set_wallclock(host.wallclock);
   gbcore_set_palette((unsigned)host.gb_palette);
   gb_core = gbcore_create(host.rom_path, NULL, 0, host.console,
                           core_sample_rate ? core_sample_rate : 32768, &cb);
   if (!gb_core)
      return -1;
   if (gb_save_load() != 0)
   {
      gbcore_shutdown(gb_core);
      gb_core = NULL;
      return -1;
   }
   return 0;
}

/* The bulk lane's datagrams waiting for the session (cartridge/save chunks,
 * acknowledgements, redundant inputs). */
static void gbl_drain_bulk(void)
{
   int st = gbl ? fe_gblink_state(gbl) : -1;
   if (host.gb_bulk_recv && (st == FE_GBLINK_SETUP ||
                             st == FE_GBLINK_RUNNING ||
                             st == FE_GBLINK_ENDING))
   {
      static uint8_t bk[1500];
      int n, guard = 256;
      while (gbl && guard-- && (n = host.gb_bulk_recv(bk, sizeof(bk))) > 0)
         fe_gblink_receive_bulk(gbl, bk, (size_t)n);
   }
}

/* fe_gblink_platform.wait: the platform pumps netdrv (and says whether the
 * display frame has time left); the bulk lane is drained here. */
static uint32_t gbl_waited_us;      /* this step's absorbed wait, for core_prof */
#ifdef DRPROF_HW
volatile unsigned fe_lp_phase;      /* fe_linkprof.h */
#endif
static int gbl_wait(void *user, uint32_t waited_us)
{
   int ok;
   unsigned was = 0;
   (void)user;
#ifdef DRPROF_HW
   was = fe_lp_phase;
#endif
   FE_LP_SET(FELP_NET);
   gbl_waited_us = waited_us;
   ok = host.gblink_wait && host.gblink_wait(waited_us);
   if (ok)
      gbl_drain_bulk();
   FE_LP_SET(was);
   (void)was;
   return ok;
}

int fe_host_gblink_window(struct fe_gblink_window *out)
{
   if (!gbl || !out)
      return 0;
   fe_gblink_take_window(gbl, out);
   return 1;
}

#ifdef GBA_LINK
/* ---- GBA link-cable sessions (docs/GBA-LINK-MIRROR.md section 14) --------
 *
 * The GB link session (fe_gblink) with fe_gbadual's machines: each console
 * runs BOTH GBAs, cabled in memory; only buttons cross the radio.  Unlike a
 * GB session the player's machine is not shut down -- it IS instance A,
 * and the session restarts it (and the partner's, instance B) from the two
 * consoles' save states.  The battery writer is held for the whole session:
 * only an agreed end lets the session's battery image reach the stick. */
static int gba_commit(void *user, const void *image, size_t size)
{
   (void)user;
   (void)image;                  /* fe_evt compiles out of a release */
   (void)size;
   fe_evt("gblink_save_commit size=%u crc=%08x ok=1 (gba: the writer "
          "persists it after the session)", (unsigned)size,
          fe_crc32(0, image, size));
   return 0;
}

/* ---- section 19: a session owns instance B's memory ---------------------
 *
 * Late mode (host.gba_dual_late): B is not loaded at boot and its static
 * block is lent to this game's ROM cache.  A session's start is a controlled
 * reload of instance A WITHOUT that block (its state and battery wait in the
 * never-lent scratch at the end of B's block), then B is loaded fresh; the
 * end is the reverse, with the block lent again.  No mid-game switch from a
 * resident to a paged cartridge: the core is torn down and loaded, exactly
 * as a boot loads it.  Any failure puts the single-player configuration back
 * from the scratch and reports, never a half-session. */
static int gba_cable_late;
void dynarec_release_translation_caches(void);     /* cpu_threaded.c */
extern uint32_t jit_cache_replay;                   /* cpu_threaded.c */
extern uint32_t gamepak_reload;                     /* gba_memory.c */
extern uint32_t gamepak_hold_bytes;                 /* gba_memory.c */
extern void *gamepak_hold;
extern uint32_t jit_cache_tier;                     /* 1 small, 2 large */
/* A's translation-cache offset for its next load (LAYOUT_PIN link cores;
 * weak: an unpinned core does not define it).  Section 20. */
extern int32_t jit_pin_at __attribute__((weak));
static void gba_pin_a(int32_t at)
{
   if (&jit_pin_at)
      jit_pin_at = at;
}
/* The session's offsets for this console's tier (decided at boot). */
static int gba_tier_ix(void)
{
   return jit_cache_tier == 2 ? 1 : 0;
}
extern uint8_t rom_translation_cache_static[];      /* mips_stub.S */
extern uint8_t *rom_translation_cache;              /* cpu_threaded.c */

/* Where instance A's state and battery wait across its reload.  On the SMALL
 * translation-cache tier (the PSP-1000), the tail of A's own static code
 * array: A's translations are discarded by the reload anyway, and its
 * emitter writes its stubs at the array's START (and nothing clears it), so
 * the tail is untouched until the state is back.  On the LARGE tier that
 * array is lent to the ROM cache, and the heap has room: a heap block, freed
 * after the restore.  Never the heap on the 1000: a block that moves
 * between the unload and the reload would shift the ROM cache. */
#define GBA_SCRATCH_SMALL (2u * 1024u * 1024u)    /* the SMALL ROM array */
static uint8_t *gba_scratch_get(size_t need, int *heap)
{
   *heap = 0;
   if (jit_cache_tier == 1 && need <= GBA_SCRATCH_SMALL / 2)
      return rom_translation_cache_static + GBA_SCRATCH_SMALL -
             ((need + 63u) & ~(size_t)63u);
   *heap = 1;
   return (uint8_t *)malloc(need);
}

/* The one-console bench with no pair running (GBA-LINK-MIRROR.md 21).  The
 * pair starts when the player's machine loads a state (fe_host_state_load);
 * a bench ini with no `load_state` therefore ran ONE machine for its whole
 * length, silently, and its core_prof was read as the pair's.
 *   gba_dual_cold = 1: start the pair before the first frame, both machines
 *     from the player's power-on machine (serialized here, loaded into both,
 *     exactly what a state file does).
 *   otherwise: say so once, loudly, when the autoload window has passed. */
static void gba_dual_bench_idle(void)
{
   static int noted;
   if (host.gba_dual_cold && frame_count == 0 && fe_gbadual_loaded())
   {
      size_t sz = retro_serialize_size();
      int ok = sz && sz <= sizeof(state_buf) && retro_serialize(state_buf, sz) &&
               retro_unserialize(state_buf, sz);
      fe_evt("gbadual_cold size=%u ok=%d", (unsigned)sz, ok);
      if (ok)
         fe_gbadual_state_loaded(state_buf, sz);
      return;
   }
   if (!noted && frame_count >= 120)
   {
      noted = 1;
      fe_evt("gbadual_idle frame=%u loaded=%d WARNING the pair is NOT running: "
             "this run is ONE machine and its core_prof is not a pair's "
             "(stage a state with load_state = 1, or set gba_dual_cold = 1)",
             frame_count, fe_gbadual_loaded());
   }
}

static void gba_dual_cfg(fe_gbadual_config *dc)
{
   memset(dc, 0, sizeof(*dc));
   dc->rom_path = host.rom_path;
   dc->script = host.gba_dual_script;
   dc->autopilot = host.gba_dual_autopilot;
   dc->mirror = host.gba_dual_mirror;
   dc->own_rom = host.gba_dual_own_rom;
   dc->lookahead = host.gba_dual_lookahead;
   dc->partner_audio = host.gba_dual_partner_audio;
   dc->sram_crc = host.gba_dual_sram_crc;
   dc->b_jit_pin_p1 = host.gba_dual_b_jit_pin_p1;
   dc->cable_fast = host.gba_dual_cable_fast;
   dc->time_us = host.time_us;
   dc->step_max_ms = host.gba_dual_step_max_ms;
   dc->hash_path = host.gba_dual_hash;
   dc->env = env_cb;
   dc->log = gbadual_log;
   dc->session = host.gba_dual_session;
}

static void gba_note(int phase, const char *what)
{
   if (host.gba_reload_note)
      host.gba_reload_note(phase, what);
}

/* Instance A out and back in, lent block per `lend`; its state and battery
 * from the scratch.  hold: heap taken before the load and handed back to the
 * caller (the session's own buffers).  0 ok; -1 the load failed; -2 no hold. */
static int gba_reload_a(int lend, const uint8_t *scr, size_t sn,
                        size_t savn, void **hold, size_t hold_n)
{
   static int a_inited = 1;          /* the boot's retro_init */
   struct retro_game_info game;
   void *sram;
   if (a_inited)
   {
      retro_unload_game();
      retro_deinit();
      dynarec_release_translation_caches();
      a_inited = 0;
   }
   fe_gbadual_lend(lend);
   gba_note(1, "Reloading the game");
   /* the same game again: the boot's translation-cache tier, and no
    * post-load reserve (the frontend's per-game allocations still stand) */
   jit_cache_replay = 1;
   gamepak_reload = 1;
   /* the hold is taken after the tier (it goes back where it was), before
    * the ROM cache takes blocks until malloc fails (init_gamepak_buffer) */
   gamepak_hold_bytes = hold ? (uint32_t)hold_n : 0u;
   retro_init();
   a_inited = 1;
   memset(&game, 0, sizeof(game));
   game.path = host.rom_path;
   {
      int ok = retro_load_game(&game);
      jit_cache_replay = gamepak_reload = 0;
      gamepak_hold_bytes = 0;
      if (hold)
         *hold = gamepak_hold;
      gamepak_hold = NULL;
      if (hold && hold_n && !*hold)
         return -2;
      if (!ok)
         return -1;
   }
   sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   if (sram && savn)
      memcpy(sram, scr + sn, savn);
   if (!retro_unserialize(scr, sn))
      return -1;
   return 0;
}

static inline unsigned ms_since(uint64_t t0)
{
   uint64_t t1 = host.time_us ? host.time_us() : 0;
   return (unsigned)((t1 - t0) / 1000u);
}

/* up = 1: single-player -> session memory (A reloaded, B loaded);
 * up = 0: the reverse.  0 ok, -1 not enough memory (single-player is back),
 * -3 single-player could not be put back (fatal: the platform exits). */
static int gba_session_mem_body(int up)
{
   size_t sn = retro_serialize_size(), savn = 0;
   uint8_t *scr;
   void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   void *hold = NULL;
   uint64_t t0 = host.time_us ? host.time_us() : 0;
   int rc, scr_heap = 0;
   (void)t0;                     /* fe_evt compiles out of a release */
   if (sram)
      savn = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
   scr = sn ? gba_scratch_get(sn + savn, &scr_heap) : NULL;
   if (!scr || !retro_serialize(scr, sn))
   {
      fe_evt("gba_sessmem FAILED up=%d reason=scratch sn=%u savn=%u heap=%d",
             up, (unsigned)sn, (unsigned)savn, scr_heap);
      if (scr_heap)
         free(scr);
      return up ? -1 : -3;
   }
   if (savn)
      memcpy(scr + sn, sram, savn);
   gba_note(0, up ? "Preparing the link" : "Ending the link");
   if (!up)
   {
      fe_gbadual_unload();
      gba_pin_a(-1);                       /* the single-player pin */
      rc = gba_reload_a(1, scr, sn, savn, NULL, 0);
      if (scr_heap)
         free(scr);
      fe_evt("gba_sessmem up=0 rc=%d ms=%u", rc, ms_since(t0));
      gba_note(rc ? -1 : 2, rc ? "The game could not be reloaded" : NULL);
      return rc ? -3 : 0;
   }
   {
      unsigned pa = host.gba_sess_pin_a_p1[gba_tier_ix()];
      gba_pin_a(pa ? (int32_t)(pa - 1u) : -1);
   }
   rc = gba_reload_a(0, scr, sn, savn, &hold,
                     host.gba_sessmem_fail == 1 ? (size_t)0x7FF00000u :
                     (size_t)(host.gba_session_reserve_kb ?
                              host.gba_session_reserve_kb : 2560u) * 1024u);
   free(hold);                   /* the session's own buffers come from here */
   if (rc == 0)
   {
      fe_gbadual_config dc;
      unsigned pb = host.gba_sess_pin_b_p1[gba_tier_ix()];
      gba_dual_cfg(&dc);
      if (pb)
         dc.b_jit_pin_p1 = pb;
      gba_note(1, "Preparing the partner's game");
      if (host.gba_sessmem_fail == 2 || fe_gbadual_load(&dc) != 0 ||
          !fe_gbadual_machines())
         rc = -4;
   }
   if (rc != 0)
   {
      /* back to exactly the single-player configuration */
      fe_evt("gba_sessmem FAILED up=1 rc=%d (restoring single-player)", rc);
      fe_gbadual_unload();
      gba_pin_a(-1);
      rc = gba_reload_a(1, scr, sn, savn, NULL, 0);
      if (scr_heap)
         free(scr);
      if (rc != 0)
      {
         gba_note(-1, "The game could not be reloaded");
         return -3;
      }
      gba_note(-1, "Not enough memory for link play");
      return -1;
   }
   if (scr_heap)
      free(scr);
   fe_evt("gba_sessmem up=1 rc=0 ms=%u a_mod8k=0x%04x", ms_since(t0),
          (unsigned)((uintptr_t)rom_translation_cache & 8191u));
   gba_note(2, NULL);
   return 0;
}

/* THE SAVE FILE ACROSS THE RELOAD.  The battery writer scans and writes the
 * cartridge RAM on its own thread (ADR-0026).  Between A's unload and the
 * moment its battery is copied back, that RAM is not the player's save
 * (retro_load_game resets it): a scan there would write it to the memory
 * stick.  So the writer is held for the whole reload -- hold first, then a
 * sync waits out any cycle already running, which reads the RAM while it is
 * still the save -- and released after the restore.  A session's own hold
 * (gba_cable_start / gba_cable_close) is kept as it was. */
static int gba_sram_hold_begin(void)
{
   int was = gba_sram_hold, rc;
   if (was)
      return sram_sync_checked();
   /* The emulation thread is parked at its frame boundary. Finish the last
    * pre-link scan and write while its RAM is still live; only this phase
    * permits a scan under the hold. No session machine may start on failure. */
   gba_sram_hold = 1;
   gba_sram_quiescing = 1;
   if (host_io)
      sram_scan_req = 1;
   else
      (void)sram_scan(0);
   rc = sram_sync_checked();
   gba_sram_quiescing = 0;
   if (rc == 0)
      rc = sram_sync_checked();
   if (rc != 0)
      gba_sram_hold = was;
   return rc;
}

static int gba_session_mem(int up)
{
   int was = gba_sram_hold, rc;
   if (gba_sram_hold_begin() != 0)
      return -3;
   rc = gba_session_mem_body(up);
   gba_sram_hold = was;
   return rc;
}

/* Harness (GBA-LINK-MIRROR.md 19): the reload alone, either way -- no radio,
 * no session.  Returns gba_session_mem's code; -2 not in late mode. */
int fe_host_gba_sessmem_cycle(int up)
{
   if (!gba_cable_late || gbl)
      return -2;
   if (up == fe_gbadual_loaded())
      return 0;
   return gba_session_mem(up);
}

static int gba_cable_start_body(int is_host, unsigned input_delay, int64_t rtc_seed)
{
   fe_gblink_platform p;
   fe_gblink_local l;
   const fe_gblink_machines *m;
   void *sram;
   size_t sn;
   if (gbl)
      return -1;
   if (gba_cable_late && !fe_gbadual_loaded())
   {
      int mrc = gba_session_mem(1);
      if (mrc != 0)
      {
         fe_evt("gblink_start FAILED reason=memory rc=%d", mrc);
         return mrc == -3 ? -3 : -2;
      }
   }
   m = fe_gbadual_machines();
   sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   sn = retro_serialize_size();
   if (!m || !sram || !sn)
      return -1;
   /* The running game at this frame boundary: the session's own state, and
    * what a failed session restores. */
   free(gbl_prelink_state);
   free(gba_prelink_sram);
   gbl_prelink_state = (uint8_t *)malloc(sn);
   gba_prelink_sram = (uint8_t *)malloc(0x20000);
   if (!gbl_prelink_state || !gba_prelink_sram ||
       !retro_serialize(gbl_prelink_state, sn))
   {
      free(gbl_prelink_state);
      free(gba_prelink_sram);
      gbl_prelink_state = NULL;
      gba_prelink_sram = NULL;
      fe_evt("gblink_start FAILED reason=gba_state size=%u", (unsigned)sn);
      return -1;
   }
   gbl_prelink_state_n = sn;
   memcpy(gba_prelink_sram, sram, 0x20000);
   memset(&p, 0, sizeof(p));
   memset(&l, 0, sizeof(l));
   p.send = gbl_send;
   p.max_payload = ND_MAX_PAYLOAD;
   p.send_room = gbl_room;
   p.bulk_per_step = gbl_bulk;
   p.commit_save = gba_commit;
   /* GB-LINK.md section 14, carried over (GBA-LINK-MIRROR.md section 15):
    * stall absorption and resend requests, each with the GB session's
    * kill switch (gblink_absorb / gblink_nack). */
   p.wait = host.gblink_wait ? gbl_wait : NULL;
   p.nack = host.gblink_nack;
   if (host.gb_bulk_send && host.gb_bulk_recv && host.gb_bulk_max &&
       (!host.gb_bulk_enable || host.gb_bulk_enable(1) == 0))
   {
      p.bulk_send = gbl_bulk_send;
      p.bulk_max = host.gb_bulk_max;
      p.bulk_rate = host.gb_bulk_rate;
      p.input_copies = host.gblink_input_copies;
   }
   l.is_host = is_host;
   l.rom_path = host.rom_path;
   l.console = FE_CONSOLE_GBA;
   l.save = gba_prelink_sram;
   l.save_size = 0x20000;
   l.state = gbl_prelink_state;
   l.state_size = sn;
   l.wallclock_now = rtc_seed ? rtc_seed
                              : (int64_t)(host.wallclock ? host.wallclock()
                                                         : time(NULL));
   l.audio_rate = core_sample_rate;
   l.input_delay = input_delay;
   /* Every 240 slot-0 frames, as the GB session (was 60): each check
    * hashes both GBAs on the emulation thread; the FINAL hash at the
    * agreed end is what protects the saves, regardless. */
   l.hash_interval = host.gblink_hash_every ? host.gblink_hash_every : 240;
   /* the pair's lookahead in scanlines (CONFIG carries it): half a frame */
   l.batch_lines = host.gba_dual_lookahead ? host.gba_dual_lookahead / 1232u
                                           : 114u;
   l.machines = m;
   l.core_mask = host.gba_core_mask;
   l.rom_sampled = gba_rom_sampled;
   gba_sram_hold = 1;
   gbl = fe_gblink_create(&p, &l);
   gbl_is_gba = gbl != NULL;
   gbl_ride_set = 0;
   gbl_last = FE_GBLINK_IDLE;
   gbl_primed = 0;
   fe_evt("gblink_lane bulk_max=%u rate=%u input_copies=%u",
          (unsigned)p.bulk_max, p.bulk_rate, p.input_copies);
   if (!gbl)
   {
      fe_evt("gblink_start FAILED reason=create");
      gba_sram_hold = 0;
      if (p.bulk_send && host.gb_bulk_enable)
         host.gb_bulk_enable(0);
      return -1;
   }
   fe_gblink_set_clock(gbl, host.time_us);
   if (fe_np_gb_peer_ready(NULL))
   {
      unsigned i;
      fe_gblink_peer(gbl, 1);
      for (i = 0; i < gbl_early_n && gbl; i++)
         fe_gblink_receive(gbl, gbl_early[i], gbl_early_len[i]);
   }
   gbl_early_n = 0;
   return 0;
}

static int gba_cable_start(int is_host, unsigned input_delay, int64_t rtc_seed)
{
   int was = gba_sram_hold, rc;
   if (gbl)
      return -1;
#ifdef REWIND
   (void)fe_host_rewind_land(0);
#endif
   if (gba_sram_hold_begin() != 0)
   {
      fe_evt("gblink_start FAILED reason=save_sync");
      return -1;
   }
   rc = gba_cable_start_body(is_host, input_delay, rtc_seed);
   if (rc != 0)
      gba_sram_hold = was;
   return rc;
}

static int gba_cable_close(void);
#endif

int fe_host_gblink_start(int is_host, unsigned input_delay, int64_t rtc_seed)
{
   fe_gblink_platform p;
   fe_gblink_local l;
   size_t n;
   uint8_t *save = NULL;
#ifdef GBA_LINK
   if (gba_cable)
      return gba_cable_start(is_host, input_delay, rtc_seed);
#endif
   if (!gb_family || gbl || !gb_core || gb_dual)
      return -1;
   /* This console's save, as the game has it now (and on disk). */
   gb_save_flush(1);
   n = gbcore_save_ram_size(gb_core);
   if (n)
   {
      save = (uint8_t *)malloc(n);
      if (!save || gbcore_save_ram_read(gb_core, save, n) != 0)
      {
         free(save);
         return -1;
      }
   }
   /* A live link: the running game, as it stands at this frame boundary. */
   free(gbl_prelink_state);
   gbl_prelink_state = NULL;
   gbl_prelink_state_n = 0;
   if (host.gblink_live)
   {
      size_t sn = gbcore_state_size(gb_core);
      gbl_prelink_state = sn ? (uint8_t *)malloc(sn) : NULL;
      if (gbl_prelink_state &&
          gbcore_state_save(gb_core, gbl_prelink_state, sn) == (long)sn)
         gbl_prelink_state_n = sn;
      else
      {
         free(gbl_prelink_state);
         gbl_prelink_state = NULL;
      }
      fe_evt("gblink_live_state size=%u", (unsigned)gbl_prelink_state_n);
   }
   memset(&p, 0, sizeof(p));
   memset(&l, 0, sizeof(l));
   p.send = gbl_send;
   p.max_payload = ND_MAX_PAYLOAD;
   p.send_room = gbl_room;
   p.bulk_per_step = gbl_bulk;
   p.find_rom = gbl_find;
   p.commit_save = gbl_commit;
   p.video = gbl_video;
   p.audio = gb_audio_batch;
   p.wait = host.gblink_wait ? gbl_wait : NULL;
   p.nack = host.gblink_nack;
   if (host.gb_bulk_send && host.gb_bulk_recv && host.gb_bulk_max &&
       (!host.gb_bulk_enable || host.gb_bulk_enable(1) == 0))
   {
      p.bulk_send = gbl_bulk_send;
      p.bulk_max = host.gb_bulk_max;
      p.bulk_rate = host.gb_bulk_rate;
      p.input_copies = host.gblink_input_copies;
   }
   fe_evt("gblink_lane bulk_max=%u rate=%u input_copies=%u",
          (unsigned)p.bulk_max, p.bulk_rate, p.input_copies);
   l.is_host = is_host;
   l.rom_path = host.rom_path;
   l.console = host.console;
   l.palette = (unsigned)host.gb_palette;
   l.save = save;
   l.save_size = n;
   l.wallclock_now = rtc_seed ? rtc_seed
                              : (int64_t)(host.wallclock ? host.wallclock()
                                                         : time(NULL));
   l.audio_rate = core_sample_rate;
   l.input_delay = input_delay;
   /* Every 240 slot-0 frames (4 s), not 60: each check hashes both
    * machines' memory on the emulation thread (~1.5 ms on a PSP even after
    * the word hash, an 8-9 ms spike before it -- GB-LINK.md section 14), and
    * an early desync report only stops a session sooner.  What protects the
    * saves is the FINAL hash, taken at the agreed end frame regardless. */
   l.hash_interval = host.gblink_hash_every ? host.gblink_hash_every : 240;
   l.hash_legacy = host.gblink_hash_legacy;
   l.batch_lines = host.gblink_batch;
   l.pace_slot0 = host.gblink_pace_slot0;
   l.state = gbl_prelink_state;
   l.state_size = gbl_prelink_state_n;
   /* The single machine goes: instance A is needed for slot 0. */
   gbcore_shutdown(gb_core);
   gb_core = NULL;
   last_frame = NULL;
   gbl = fe_gblink_create(&p, &l);
   free(save);
   gbl_last = FE_GBLINK_IDLE;
   gbl_primed = 0;
   if (!gbl)
   {
      fe_evt("gblink_start FAILED reason=create");
      if (p.bulk_send && host.gb_bulk_enable)
         host.gb_bulk_enable(0);
      gb_single_boot();
      return -1;
   }
   fe_gblink_set_clock(gbl, host.time_us);
   if (fe_np_gb_peer_ready(NULL))
   {
      unsigned i;
      fe_gblink_peer(gbl, 1);
      for (i = 0; i < gbl_early_n && gbl; i++)
         fe_gblink_receive(gbl, gbl_early[i], gbl_early_len[i]);
   }
   gbl_early_n = 0;
   return 0;
}

int fe_host_gblink_active(void) { return gbl != NULL; }
static int gbl_repeat;              /* inside a stall's re-present */
int fe_host_gblink_stalled(void)
{
   return gbl_repeat;
}
void fe_host_gblink_hold(int on) { gbl_hold = on ? 1 : 0; }
int fe_host_gblink_state(void) { return gbl ? fe_gblink_state(gbl) : -1; }
void fe_host_set_fast_link(int on)
{
#ifdef GBA_LINK
   host.gba_core_mask = (host.gba_core_mask & ~FE_GBLINK_MASK_INPUT_BATCH) |
                        (on ? FE_GBLINK_MASK_INPUT_BATCH : 0u);
#else
   (void)on;                    /* no GBA cable in this build */
#endif
}
int fe_host_fast_link(int *source)
{
   (void)gbl_ride_set;
   if (!gbl || !gbl_is_gba)
      return -1;
   return fe_gblink_fast_link(gbl, source);
}

void fe_host_gblink_status(char *l1, size_t c1, char *l2, size_t c2)
{
   fe_gblink_status(gbl, l1, c1, l2, c2);
}

void fe_host_gblink_progress(int *p1, int *p2)
{
   fe_gblink_status_progress(gbl, p1, p2);
}

int fe_host_gblink_facts(struct fe_gblink_progress *out)
{
   fe_gblink_get_progress(gbl, out);
   return gbl != NULL;
}

void fe_host_gba_cable_rom_sampled(int on)
{
#ifdef GBA_LINK
   gba_rom_sampled = on ? 1 : 0;
#else
   (void)on;
#endif
}

int fe_host_gba_cable_ready(void)
{
   return GBA_CABLE;
}

void fe_host_gblink_request_end(void)
{
   if (gbl)
      fe_gblink_request_end(gbl);
}

static void gblink_stats_evt(void);

int fe_host_gblink_close(void)
{
   if (!gbl)
      return -1;
#ifdef GBA_LINK
   if (gba_cable)
   {
      int crc = gba_cable_close();
      if (gba_cable_late && fe_gbadual_loaded() && gba_session_mem(0) != 0)
         return -3;             /* the platform cannot continue: exit */
      return crc;
   }
#endif
   /* The session's totals, always: with the periodic reporting off
    * (telemetry_period) the heartbeat never printed them. */
   gblink_stats_evt();
   /* t_ms (this console's clock): close -> resume is how long the game
    * takes to come back (the single machine re-reads its cartridge), the
    * hw4 Y ending's 3000-vs-1000 gap. */
   fe_evt("gblink_close state=%d error=%s t_ms=%llu", fe_gblink_state(gbl),
          fe_gblink_error_text(fe_gblink_error(gbl)),
          (unsigned long long)(host.time_us ? host.time_us() / 1000u : 0));
   {
      fe_gblink_stats st;
      fe_gblink_get_stats(gbl, &st);
      fe_evt("gblink_bulk_stats chunk=%u sent=%u resent=%u rx=%u dup=%u "
             "bad=%u rate=%u", st.bulk_chunk, st.bulk_sent, st.bulk_resent,
             st.bulk_rx, st.bulk_dup, st.bulk_bad, st.bulk_rate);
   }
   {
      /* DONE: continue from the agreed end (its battery image is on disk,
       * clock re-based to this PSP).  Anything else: back to the moment
       * the link began -- nothing of the session was saved, so nothing of
       * it may continue either (the game would save it later itself). */
      size_t sn = 0;
      const void *fs = fe_gblink_final_state(gbl, &sn);
      uint8_t *state = NULL;
      int rc;
      const char *how = "restart";
      if (host.gblink_resume_boot)
         how = "boot_from_battery";
      else if (fs && sn && (state = (uint8_t *)malloc(sn)) != NULL)
      {
         memcpy(state, fs, sn);
         how = "live_end";
      }
      else if (gbl_prelink_state)
      {
         state = gbl_prelink_state;
         sn = gbl_prelink_state_n;
         gbl_prelink_state = NULL;
         how = "restore_prelink";
      }
      fe_gblink_destroy(gbl);
      gbl = NULL;
      last_frame = NULL;
      gbl_sessions++;
      gbl_hold = 0;
      if (host.gb_bulk_enable)
         host.gb_bulk_enable(0);
      rc = gb_single_boot();
      if (rc == 0 && state)
      {
         rc = gbcore_state_load(gb_core, state, sn);
         if (rc != 0)
         {
            /* the battery image on disk is still right: power on from it */
            fe_evt("gblink_resume FAILED state=%u", (unsigned)sn);
            gbcore_shutdown(gb_core);
            gb_core = NULL;
            rc = gb_single_boot();
            how = "restart";
         }
      }
      free(state);
      free(gbl_prelink_state);
      gbl_prelink_state = NULL;
      FE_EVT_ONLY(how);
      fe_evt("gblink_resume how=%s rc=%d t_ms=%llu", how, rc,
             (unsigned long long)(host.time_us ? host.time_us() / 1000u : 0));
      return rc;
   }
}

/* The autopilot takes one look per frame of THIS console's machine: at
 * power-on, then after each completed frame -- never while a partner input
 * is late -- so a script sees exactly what it would in linkplay/sesssim. */
int fe_host_autopilot_gate(void)
{
   int st;
   if (!gbl)
      return !host.gblink_script_waits || (gbl_sessions && !gbl_hold);
   st = fe_gblink_state(gbl);
   if (st != FE_GBLINK_RUNNING && st != FE_GBLINK_ENDING)
      return 0;
   if (!gbl_primed)
   {
      gbl_primed = 1;
      return 1;
   }
   return gbl_last == FE_GBLINK_FRAME;
}

void fe_host_session_reset(void)
{
   if (options_boot_saved)
      memcpy(options, options_boot, sizeof(options));
   options_dirty = 0;
   /* The save writer's requests belong to the game that was just shut down
    * (its flush and sync have run, the writer thread is stopped): a rewrite
    * or scan still flagged must not be carried out on the next game. */
   sram_full_req = 0;
   sram_retry_full = 0;
   sram_scan_req = 0;
}

int fe_host_boot(const fe_host_config *cfg)
{
   struct retro_game_info game;
   struct retro_system_av_info av;

   if (!options_boot_saved)
   {
      memcpy(options_boot, options, sizeof(options));
      options_boot_saved = 1;
   }
   host = *cfg;
#ifdef GBA_LINK
   /* Per game, decided below for a GBA game only: a process that returns to
    * the game list in place (docs/INPROC-RETURN.md) must not carry the last
    * GBA game's cable mode into a GB game, whose link start would otherwise
    * be routed to gba_cable_start. */
   gba_cable = 0;
   gba_cable_late = 0;
#endif
   hb_every = cfg->heartbeat_frames ? cfg->heartbeat_frames
                                    : HEARTBEAT_INTERVAL;
   frame_count = 0;
   last_frame = NULL;
   frames_rendered = 0;
   frames_skipped = 0;
   fps_last_frames = fps_last_rendered = fps_last_skipped = 0;
   gb_save_crc_valid = 0;
   gb_skip_frame = 0;

   gb_family = host.console == FE_CONSOLE_GB || host.console == FE_CONSOLE_GBC;
   if (gb_family)
   {
      gbcore_callbacks_t callbacks;
      char title[17] = "";
      unsigned rate = 32768;
      gbcore_set_color_correction(!host.gbc_raw_colors);
      if (host.boot_status) host.boot_status("Preparing GB emulator");
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.video = gb_video_frame;
      callbacks.audio_batch = gb_audio_batch;
      callbacks.serial_transfer = gb_serial_transfer;
      gb_link_reset();
      fe_np_gb_set_receive(gb_link_receive_message, NULL);
      fe_np_gb_set_peer_callback(gb_link_peer_event, NULL);
      /* Before create: the core takes its clock base and chooses DMG, SGB
       * or CGB behaviour (the palette decides SGB colours) as it boots. */
      if (host.gb_dual_rom)
      {
         /* The pair takes the cable: no ad-hoc byte link beside it. */
         gbdual_config_t dc;
         const char *dot = strrchr(host.gb_dual_rom, '.');
         memset(&dc, 0, sizeof(dc));
         dc.machine[0].rom_path = host.rom_path;
         dc.machine[0].console = host.console;
         dc.machine[0].palette = (unsigned)host.gb_palette;
         dc.machine[0].wallclock = host.wallclock;
         dc.machine[0].callbacks.video = gb_video_frame;
         dc.machine[0].callbacks.audio_batch = gb_audio_batch;
         dc.machine[1].rom_path = host.gb_dual_rom;
         dc.machine[1].console = (dot && (dot[3] == 'c' || dot[3] == 'C'))
                                 ? FE_CONSOLE_GBC : FE_CONSOLE_GB;
         dc.machine[1].palette = (unsigned)host.gb_palette;
         dc.machine[1].wallclock = host.wallclock;
         dc.machine[1].headless = 1;
         dc.audio_rate = rate;
         dc.link = 1;
         dc.pace_slot = 0;
         dc.batch_lines = host.gb_dual_batch;
         gb_dual = gbdual_create(&dc);
         gb_core = gb_dual ? gbdual_core(gb_dual, 0) : NULL;
         fe_evt("gb_dual_bench rom=%s console=%s headless=1 mirror=%d "
                "batch=%u ok=%d", host.gb_dual_rom,
                dc.machine[1].console == FE_CONSOLE_GBC ? "GBC" : "GB",
                host.gb_dual_mirror, dc.batch_lines ? dc.batch_lines : 1u,
                gb_dual != NULL);
         if (gb_dual && host.gb_dual_state)
         {
            FILE *sf = fopen(host.gb_dual_state, "rb");
            size_t n = sf ? fread(state_buf, 1, sizeof(state_buf), sf) : 0;
            int rc = -1;
            FE_EVT_ONLY(rc);
            if (sf)
               fclose(sf);
            if (n)
               rc = gbdual_api(1)->state_load(gbdual_core(gb_dual, 1),
                                              state_buf, n);
            fe_evt("gb_dual_bench state=%s size=%u rc=%d", host.gb_dual_state,
                   (unsigned)n, rc);
         }
      }
      else
      {
         /* Every static byte of the instance back to what a fresh boot
          * holds, as a link session's machines start: gb_reset leaves the
          * sound and some CGB registers from the last game, and the PSP
          * now loads a second game in the same process (the in-process
          * return to the game list, docs/INPROC-RETURN.md).  A no-op
          * difference on the first game of a process. */
         (void)gbcore_power_on();
         gbcore_set_wallclock(host.wallclock);
         gbcore_set_palette((unsigned)host.gb_palette);
         gb_core = gbcore_create(host.rom_path, NULL, 0, host.console, rate,
                                 &callbacks);
         if (gb_core && host.gb_bench_headless)
         {
            /* Milestone-1 bench: the one machine as a link session's
             * partner machine runs -- no picture, no synthesis -- to
             * measure what headless costs on its own. */
            gbcore_set_headless(gb_core, 1);
            fe_evt("gb_bench headless=1");
         }
      }
      if (!gb_core)
      {
         fe_log("gbcore_create FAILED for %s console=%d", host.rom_path,
                (int)host.console);
         return -1;
      }
      core_sample_rate = gbcore_sample_rate(gb_core);
      gbcore_rom_title(gb_core, title, sizeof(title));
      fe_evt("gb_rom_loaded console=%s title=%s", host.console == FE_CONSOLE_GBC ?
             "GBC" : "GB", title);
      fe_evt("av_info fps=59.7275 rate=%u w=%u h=%u", core_sample_rate,
             gbcore_frame_width(gb_core), gbcore_frame_height(gb_core));
      if (host.boot_status) host.boot_status("Loading GB save");
      if (gb_save_load() != 0)
      {
         if (gb_dual)
            gbdual_destroy(gb_dual);
         else
            gbcore_shutdown(gb_core);
         gb_dual = NULL;
         gb_core = NULL;
         return -1;
      }
      fps_last_us = host.time_us ? host.time_us() : 0;
      fe_evt("gb_backend ready console=%d", (int)host.console);
      return 0;
   }

   if (host.console != FE_CONSOLE_GBA)
      return -1;

   retro_set_environment(env_cb);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_sample_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);

   if (host.boot_status) host.boot_status("Preparing emulator");
#ifdef GBA_LINK
   /* GBA-LINK-MIRROR.md 19: instance B's static state belongs to a cable
    * session.  Until one starts it is lent to this game's ROM cache. */
   fe_gbadual_boot();
   fe_gbadual_lend((!host.gba_dual || host.gba_dual_late) &&
                   !host.gba_link_nolend);
   fe_evt("gba_link_lend on=%d bytes=%u", fe_gbadual_lent(),
          (unsigned)fe_gbadual_lendable());
#endif
   retro_init();

   if (host.boot_status) host.boot_status("Reading BIOS");
   log_bios_evt();

   memset(&game, 0, sizeof(game));
   game.path = host.rom_path;
   if (host.boot_status) host.boot_status("Reading ROM");
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
   /* Integers, not %f: newlib's float formatting keeps dtoa's Bigint cache
    * for the life of the process, allocated at its first use -- here, in
    * the middle of the first game, where the in-process return to the game
    * list measures the heap (docs/INPROC-RETURN.md). */
   {
      unsigned fps_x10k = (unsigned)(av.timing.fps * 10000.0 + 0.5);
      FE_EVT_ONLY(fps_x10k);
      fe_evt("av_info fps=%u.%04u rate=%u w=%u h=%u", fps_x10k / 10000u,
             fps_x10k % 10000u, (unsigned)av.timing.sample_rate,
             av.geometry.base_width, av.geometry.base_height);
   }

   if (host.boot_status) host.boot_status("Loading save");
   if (sram_load() != 0)
   {
      retro_unload_game();
      retro_deinit();
      return -1;
   }
#ifdef GBA_LINK
   if (host.gba_dual && host.gba_dual_late && host.gba_dual_session)
   {
      /* Section 19: B is loaded when a session starts, not here. */
      gba_cable = 1;
      gba_cable_late = 1;
      gb_link_reset();
      fe_np_gb_set_receive(gb_link_receive_message, NULL);
      fe_np_gb_set_peer_callback(gb_link_peer_event, NULL);
      fe_evt("gba_cable ready late=1 (B loads at session start)");
   }
   else if (host.gba_dual)
   {
      fe_gbadual_config dc;
      gba_dual_cfg(&dc);
      if (host.boot_status) host.boot_status("Preparing the link partner");
      if (fe_gbadual_load(&dc) == 0 && host.gba_dual_session)
      {
         gba_cable = 1;
         gb_link_reset();
         fe_np_gb_set_receive(gb_link_receive_message, NULL);
         fe_np_gb_set_peer_callback(gb_link_peer_event, NULL);
         fe_evt("gba_cable ready (link-cable session available)");
      }
   }
#endif
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

   FE_EVT_ONLY(dskp); FE_EVT_ONLY(emu_x100); FE_EVT_ONLY(ren_x100);

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
 * 2 MiB of generated code on the small cache tier (10 MiB on the large one
 * a 64 MiB PSP gets) and forces re-translation, and (b) a ROM page fault, which is a 32 KiB read from the
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

void fe_host_gb_palette_set(int id)
{
   host.gb_palette = id;
   gbcore_set_palette((unsigned)id);
}

int fe_host_gb_palette_count(void)
{
   return GBCORE_PALETTE_COUNT;
}

const char *fe_host_gb_palette_name(int id)
{
   return gbcore_palette_name((unsigned)id);
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

   FE_EVT_ONLY(prev_us); FE_EVT_ONLY(d);
   FE_EVT_ONLY(prev_rf); FE_EVT_ONLY(prev_ff); FE_EVT_ONLY(prev_sf);
   FE_EVT_ONLY(prev_df); FE_EVT_ONLY(prev_pl);
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

/* BADJUMP_SAFE (a CORE flag, so invisible to this file's defines): the
 * dynarec caught the guest jumping to an address that is not code.  It can
 * only point the CPU at the reset vector from inside the dispatcher, which is
 * not a reset -- the BIOS then boots with the game's IRQs, timers and sound
 * DMA still live (black screen over a looping sound buffer).  The core raises
 * this flag instead, and the real reset happens here, between frames.  Weak,
 * so a core built without the guard links and never resets. */
extern volatile uint32_t badjump_reset_pending __attribute__((weak));
extern uint32_t badjump_last_pc __attribute__((weak));
static unsigned guest_faults;

static void fe_host_guest_fault_check(void)
{
   if (!&badjump_reset_pending || !badjump_reset_pending)
      return;
   badjump_reset_pending = 0;
   guest_faults++;
   fe_evt("guest_fault n=%u pc=%08x frame=%u action=reset", guest_faults,
          &badjump_last_pc ? (unsigned)badjump_last_pc : 0u, frame_count);
   retro_reset();
}

unsigned fe_host_guest_faults(void)
{
   return guest_faults;
}

/* The frameskip the platform asks of the GBA core (fast-forward's CPU
 * Unlimited mode sets gpsp_frameskip=fixed_interval) applies to the GB core
 * too: skipped frames are emulated and heard but not drawn, and reach the
 * platform as the NULL "nothing new" frame.  0 = draw every frame. */
static unsigned gb_frameskip_interval(void)
{
   const char *mode = NULL, *interval = NULL;
   size_t i;
   for (i = 0; i < sizeof(options) / sizeof(options[0]); i++)
   {
      if (strcmp(options[i].key, "gpsp_frameskip") == 0)
         mode = options[i].value;
      else if (strcmp(options[i].key, "gpsp_frameskip_interval") == 0)
         interval = options[i].value;
   }
   if (!mode || strcmp(mode, "fixed_interval") != 0 || !interval)
      return 0;
   return (unsigned)atoi(interval);
}

static void gblink_stats_evt(void)
{
   fe_gblink_stats s;
   fe_gblink_get_stats(gbl, &s);
   fe_evt("gblink_stats slot=%d delay=%u f0=%llu f1=%llu stalls=%u "
          "streak_max=%u hashes=%u/%u last_hash=%u:%016llx episodes=%u "
          "inputs_fast=%u inputs_ordered=%u nacks=%u resends=%u "
          "inputs_resent=%u", s.local_slot,
          s.input_delay, (unsigned long long)s.frames[0],
          (unsigned long long)s.frames[1], s.stalls, s.stall_streak_max,
          s.hashes_matched, s.hashes_sent, s.last_hash_frame,
          (unsigned long long)s.last_hash, s.stall_episodes, s.inputs_fast,
          s.inputs_ordered, s.nacks, s.resends, s.inputs_resent);
}

static void gblink_frame(uint64_t core_t0)
{
   uint32_t pad = (host.input_bitmask ? host.input_bitmask() : 0) |
                  injected_mask;
   int st = fe_gblink_state(gbl);
   gbl_drain_bulk();
   gbl_waited_us = 0;
   gbl_last = fe_gblink_step(gbl, gb_buttons_from_retro(pad));
   if (gbl_last == FE_GBLINK_STALL && host.video_frame)
   {
      gbl_repeat = 1;
      host.video_frame(NULL, 160, 144, last_pitch);
      gbl_repeat = 0;
   }
   if (gbl_last != FE_GBLINK_FRAME)
   {
      if (st != fe_gblink_state(gbl))
         gblink_stats_evt();
      return;
   }
   if (core_t0 && host.time_us)
   {
      /* An absorbed wait is idle time, not work (core_prof's `core` is the
       * frame's work, as in every other mode). */
      uint32_t elapsed = (uint32_t)(host.time_us() - core_t0);
      elapsed = elapsed > gbl_waited_us ? elapsed - gbl_waited_us : 0;
      core_run_us_total += elapsed;
      core_run_calls++;
      if (elapsed > core_run_max_us) core_run_max_us = elapsed;
      if (elapsed > core_win_max_us) core_win_max_us = elapsed;
   }
   frame_count++;
   if ((frame_count % hb_every) == 0)
   {
      fe_evt("heartbeat frames=%u t_us=%llu", frame_count,
             (unsigned long long)(host.time_us ? host.time_us() : 0));
      fps_evt();
      core_prof_evt();
      gblink_stats_evt();
   }
}

#ifdef GBA_LINK
static void gblink_stats_evt(void);

static void gba_cable_frame(uint64_t core_t0)
{
   uint32_t pad = (host.input_bitmask ? host.input_bitmask() : 0) |
                  injected_mask;
   int st = fe_gblink_state(gbl);
#ifdef DRPROF_HW
   unsigned lp_was = fe_lp_phase;   /* the caller's loop region */
#endif
   FE_LP_SET(FELP_NET);
   gbl_drain_bulk();
   gbl_waited_us = 0;
   FE_LP_SET(FELP_SESS);
   gbl_last = fe_gblink_step(gbl, fe_gbadual_keys_from_retro(pad));
   if (!gbl_ride_set && fe_gblink_state(gbl) == FE_GBLINK_RUNNING)
   {
      /* RUNNING: both consoles now send every frame or two, so an ack has
       * a packet to ride on.  Not before (setup is one-directional) and
       * not after gba_cable_close. */
      gbl_ride_set = 1;
      if (gbl_is_gba)
      {
         fe_np_ack_ride_session(1);
         fe_evt("net_tuning ack_ride_us=%u at=running",
                (unsigned)fe_np_ack_ride_now());
      }
   }
#ifdef DRPROF_HW
   FE_LP_SET(lp_was);
#endif
   if (gbl_last != FE_GBLINK_FRAME)
   {
      if (st != fe_gblink_state(gbl))
         gblink_stats_evt();
      return;
   }
   if (core_t0 && host.time_us)
   {
      /* an absorbed wait is idle time, not work (as gblink_frame) */
      uint32_t elapsed = (uint32_t)(host.time_us() - core_t0);
      elapsed = elapsed > gbl_waited_us ? elapsed - gbl_waited_us : 0;
      core_run_us_total += elapsed;
      core_run_calls++;
      if (elapsed > core_run_max_us) core_run_max_us = elapsed;
      if (elapsed > core_win_max_us) core_win_max_us = elapsed;
   }
   frame_count++;
   if ((frame_count % hb_every) == 0)
   {
      fe_evt("heartbeat frames=%u t_us=%llu", frame_count,
             (unsigned long long)(host.time_us ? host.time_us() : 0));
      fps_evt();
      core_prof_evt();
      gblink_stats_evt();
      fe_gbadual_stats_evt(gbadual_evt);
   }
}

/* DONE: the player's machine continues from the agreed end (its state and
 * battery image as they stood there; the writer then persists the image).
 * Anything else: back to the moment the link began, battery included. */
static int gba_cable_close(void)
{
   fe_np_ack_ride_session(0);
   gbl_is_gba = 0;
   size_t sn = 0, vn = 0;
   const void *fs = fe_gblink_final_state(gbl, &sn);
   const void *fv = fe_gblink_final_save(gbl, &vn);
   void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   uint8_t *state = NULL, *bat = NULL;
   const char *how = "restore_prelink";
   int rc = 0, carry_on = 0;
   gblink_stats_evt();
   fe_gbadual_stats_evt(gbadual_evt);
   fe_evt("gblink_close state=%d error=%s t_ms=%llu", fe_gblink_state(gbl),
          fe_gblink_error_text(fe_gblink_error(gbl)),
          (unsigned long long)(host.time_us ? host.time_us() / 1000u : 0));
   {
      fe_gblink_stats st;
      fe_gblink_get_stats(gbl, &st);
      fe_evt("gblink_bulk_stats chunk=%u sent=%u resent=%u rx=%u dup=%u "
             "bad=%u rate=%u", st.bulk_chunk, st.bulk_sent, st.bulk_resent,
             st.bulk_rx, st.bulk_dup, st.bulk_bad, st.bulk_rate);
   }
   /* LOST LINK: EACH PLAYER CARRIES ON (owner, 2026-10-03; docs/
    * GBA-LINK-UI.md).  Once the machines ran, the player's machine is their
    * own live game: a session the partner left (or that went silent, or
    * that the partner abandoned for any reason but a desync) does not
    * rewind it.  It continues from where it stands -- fe_gbadual's stop
    * first completes its frame -- and its battery is saved as usual.  A
    * desync, or a failure before the machines ran, still goes back to the
    * moment the link began (nothing of the session is kept). */
   if (fe_gblink_state(gbl) == FE_GBLINK_FAILED)
   {
      fe_gblink_progress pr;
      fe_gblink_get_progress(gbl, &pr);
      carry_on = pr.ran &&
                 (pr.error == FE_GBLINK_E_PEER_LOST ||
                  pr.error == FE_GBLINK_E_NO_PEER ||
                  (pr.error == FE_GBLINK_E_PEER_ABORT &&
                   pr.peer_reason != FE_GBLINK_E_DESYNC));
   }
   if (fs && sn && fv && vn == 0x20000 &&
       (state = (uint8_t *)malloc(sn)) != NULL &&
       (bat = (uint8_t *)malloc(vn)) != NULL)
   {
      memcpy(state, fs, sn);
      memcpy(bat, fv, vn);
      how = "live_end";
   }
   /* The pair ends here (both machines leave cable mode). */
   fe_gblink_destroy(gbl);
   gbl = NULL;
   gbl_sessions++;
   gbl_hold = 0;
   if (host.gb_bulk_enable)
      host.gb_bulk_enable(0);
   if (!bat && carry_on)
   {
      /* A as it stands after its frame; its battery image is the live one. */
      free(state);
      state = NULL;
      how = "carry_on";
   }
   else if (!bat)
   {
      free(state);
      state = gbl_prelink_state;
      sn = gbl_prelink_state_n;
      gbl_prelink_state = NULL;
      bat = gba_prelink_sram;
      gba_prelink_sram = NULL;
   }
   if (state && !retro_unserialize(state, sn))
   {
      fe_evt("gblink_resume FAILED state=%u", (unsigned)sn);
      rc = -1;
   }
   if (bat && sram)
   {
      /* The session's save becomes the cartridge's.  This is a write like
       * the game's own: count it, or nothing knows the stick is behind and
       * a console switched off within the writer's 5 s cadence after a
       * finished trade lost it (review S5).  The scan is asked for where
       * the hold is released, below. */
      memcpy(sram, bat, 0x20000);
      if (&backup_write_gen)
         backup_write_gen++;
   }
   free(state);
   free(bat);
   free(gbl_prelink_state);
   free(gba_prelink_sram);
   gbl_prelink_state = NULL;
   gba_prelink_sram = NULL;
   gba_sram_hold = 0;
   fe_host_sram_request_scan();   /* the committed save goes to the stick now */
   FE_EVT_ONLY(how);
   fe_evt("gblink_resume how=%s rc=%d t_ms=%llu", how, rc,
          (unsigned long long)(host.time_us ? host.time_us() / 1000u : 0));
   return rc;
}
#endif

void fe_host_skip_override(int skip)
{
   skip_override = skip < 0 ? -1 : (skip ? 1 : 0);
   if (&frontend_skip_override)
      frontend_skip_override = skip_override;
}

#ifdef REWIND
/* ---- rewind glue (PROTOTYPE; fe_rewind.h, docs/REWIND.md) -----------------
 * fe_rewind.c is the mechanism and knows no GBA; this is the GBA machine it
 * drives.  The snapshot is the save-state image, whole -- the same bytes a
 * save state holds, so "everything a save state needs" is true by
 * construction -- plus the two things a save state leaves out and a rewind
 * cannot (gpsp_rewind_gather in libretro/libretro.c).
 *
 * The cartridge save (flash/EEPROM/SRAM contents) is NOT in that image and
 * a rewind never changes it, in memory or in the file: see "the save is
 * permanent" below.
 *
 * The rewind timeline (fe_rewind_frame_now) is NOT frame_count: frame_count
 * keeps counting frames run, as every other user of it expects, while the
 * timeline goes back when the player does. */
static fe_host_rewind_opts rw_opt;
static void    *rw_mem;
static void     rw_win_mark(void);
static void     rw_gesture_mark(void);
static void     rw_gesture_evt(void);
static uint8_t *rw_hash_state;      /* test: scratch for the per-frame hash */
static uint32_t rw_seen_gen;        /* core backup_write_gen last seen */
static int      rw_ff;              /* platform is fast-forwarding */
static uint32_t rw_redo_high;       /* test: redo logged inputs up to here */
static unsigned rw_barriers_cheat;
static uint64_t rw_tick_us;
static unsigned rw_tick_n, rw_tick_max_us;
static uint16_t rw_present[FE_GBA_WIDTH * FE_GBA_HEIGHT];
static int      rw_present_valid;

/* The core's rewind image (libretro/libretro.c): the save state plus the
 * two things it leaves out that a rewind cannot. */
extern size_t gpsp_rewind_state_size(void);
extern bool   gpsp_rewind_gather(void *data, size_t size);
extern bool   gpsp_rewind_restore(const void *data, size_t size);
extern uint32_t gpsp_rewind_light;
/* Measurement: translated code emitted while re-running after a restore
 * (cpu_threaded.c).  Weak: an interpreter-only core has no such thing. */
extern void gpsp_rewind_jit_used(uint32_t *rom, uint32_t *ram)
   __attribute__((weak));
static uint32_t rw_jit_rom0, rw_jit_ram0;
static int      rw_jit_open;
static uint64_t rw_jit_rom_emit, rw_jit_ram_emit;

static void rw_jit_close(void)
{
   uint32_t rom, ram;
   if (!rw_jit_open || !gpsp_rewind_jit_used)
      return;
   gpsp_rewind_jit_used(&rom, &ram);
   if (rom > rw_jit_rom0) rw_jit_rom_emit += rom - rw_jit_rom0;
   if (ram > rw_jit_ram0) rw_jit_ram_emit += ram - rw_jit_ram0;
   rw_jit_open = 0;
}

/* ---- how the state reaches a snapshot -------------------------------------
 * Two ways, one binary (fe_host_rewind_opts.inplace):
 *
 *   image    the whole save-state image is serialised into a buffer and that
 *            buffer is compared with the reference.  One span.  The first
 *            version, kept as the baseline to measure against.
 *   in place the six big arrays (IWRAM, EWRAM, VRAM, OAM, palette, I/O; 387
 *            of the image's 459 KiB) are compared where they lie in the
 *            machine's memory: nothing is copied.  Only the rest of the
 *            image (a few KiB) and the sound in flight are serialised, into
 *            two small buffers.  Eight spans.  With vram_dirty, VRAM pages
 *            nothing wrote to since the last snapshot are not even read.
 *
 * A restore is the mirror image: the arrays are copied back from the
 * reference and the unchanged save-state loader does the rest. */
typedef struct { uint32_t bytes; uint8_t *live; } rw_arr_t;
extern uint32_t gpsp_rewind_layout(uint8_t *scratch, rw_arr_t arr[6]);
extern void     gpsp_rewind_rest_gather(uint8_t *scratch, uint8_t *rest);
extern bool     gpsp_rewind_rest_restore(uint8_t *scratch, const uint8_t *rest,
                                         int keep_ram_code);
extern int      gpsp_rewind_arrays_restore(const uint8_t *ref,
                                           const uint8_t *ewram256,
                                           const uint8_t *vram96,
                                           uint32_t *copied);
extern void     gpsp_rewind_map_peek(uint8_t *ewram256, uint8_t *vram96);
extern void     gpsp_rewind_map_clean(void);
extern void     gpsp_rewind_marks_want(int on);
extern int      gpsp_rewind_marks_ok(void);
extern void     gpsp_rewind_clock_mode(int frames);
extern void     gpsp_rewind_clock_sync(void);
extern long long gpsp_rewind_clock_time(const long long *set);
extern uint32_t gpsp_rewind_audio_bytes(void);
extern uint32_t gpsp_rewind_audio_gather(uint8_t *dst, uint32_t stale);
extern void     gpsp_rewind_audio_restore(const uint8_t *src);
extern int      gpsp_rewind_take_dirty(uint8_t *ewram256, uint8_t *vram96);
extern void     gpsp_rewind_gprof(uint32_t out[4], uint64_t (*clock)(void));
extern uint32_t gba_rewind_cheat_gen;
extern uint32_t gba_rewind_serial_gen;
extern uint32_t gba_rewind_backup_ro, gba_rewind_backup_ro_hits;
extern uint32_t gba_rewind_bk_acc;
extern uint8_t *gba_rewind_rlog;
extern uint32_t gba_rewind_rlog_mask, gba_rewind_rlog_pos, gba_rewind_rlog_end;
extern uint32_t gba_rewind_rlog_served, gba_rewind_rlog_miss;
extern uint32_t gba_rewind_rlog_mutant;
extern uint32_t gba_rewind_backup_busy(void);

#define RW_REST_CAP  (16u * 1024u)   /* the rest of the image is ~4 KiB */
#define RW_AUD_PAGE  10              /* dirty-map page of the sound span */
static fe_rewind_span rw_spans[10];
static uint32_t rw_nspans;
static uint8_t *rw_buf;              /* scratch image, then rest, then sound */
static size_t   rw_buf_bytes;
static uint8_t *rw_scratch, *rw_rest, *rw_aud;
static uint32_t rw_rest_bytes, rw_aud_bytes, rw_aud_used;
static rw_arr_t rw_arr[6];
/* ---- the save is permanent --------------------------------------------------
 * The owner's rule (docs/REWIND.md 17): a rewind takes back the game, never
 * the save, like a real cartridge.  The cartridge save is not in a
 * snapshot; nothing here reads, writes, holds back or replaces the .sav, and
 * the flush path does not know a rewind exists.  As built:
 *   - a save write does not end the history;
 *   - frames re-run for a rewind run with the save write-protected
 *     (gba_rewind_backup_ro): the chip's command state runs, the bytes stay;
 *   - what such a frame READS of the save comes from the read log (the
 *     bytes the first pass read, gba_memory.h), because the contents may be
 *     newer now.  The log's position is the last 8 bytes of a snapshot's
 *     first span (rw_pos_cur / rw_pos_off).  The log is a ring: history
 *     that reaches back beyond it is dropped (rw_rlog_floor);
 *   - a frame in which the game touched the chip, or left it in the middle
 *     of something, is reported to the core as FE_RW_IN_BUSY and one that
 *     changed the contents as FE_RW_IN_CHANGE; fe_rewind never lets play
 *     continue from inside such a stretch once the save has changed after
 *     it (fe_rewind.h, "where a rewind may not stop").
 * rw_quiet is now the serial port's alone.
 * rw_floor_frame / rw_floor_why: where the history last started and why,
 * for the readout at the stop. */
static uint32_t rw_quiet;            /* frames still to pass after a write */
static int      rw_quiet_why;        /* ...and what it was (FE_RW_LIMIT_*) */
#define RW_RLOG_CHUNKS 32
static uint32_t rw_rlog_bytes = 131072u;
static uint8_t *rw_rlog;
static uint32_t rw_rlog_cframe[RW_RLOG_CHUNKS];  /* frame that entered chunk c */
static uint32_t rw_rlog_chunk;       /* chunk the log's end is in */
static uint32_t *rw_pos_cur;         /* where gather puts the log position */
static uint32_t rw_pos_off;          /* ...and its offset in an image */
static uint32_t rw_bk_quiet = 0xFFFF;/* frames since the chip was last busy */
/* Frames since the game last WROTE its save or left the chip in the middle
 * of something.  A rewind is not begun inside a save in progress: the save
 * is permanent, so going back from the middle of one would leave it half
 * written for good (a cartridge pulled while "Saving...").  The control
 * simply does nothing until the save has been finished for save_quiet
 * frames. */
static uint32_t rw_wr_quiet = 0xFFFF;
static unsigned rw_denied_saving;
static int      rw_bk_carry;         /* the history has just (re)started */
static unsigned rw_bk_busy_frames, rw_rlog_trims, rw_rlog_resets;
/* Quiet frames between two busy ones, by length (what N has to cover):
 * 1-4, 5-8, 9-15, 16-30, 31-60, 61-120, 121-300, more. */
static unsigned rw_bk_gaps[8];
static uint32_t rw_rlog_pos_seen;
/* The serial port is the same kind of thing (serial.c,
 * gba_rewind_serial_gen): the adapter or cable on the other end is not in a
 * snapshot.  A frame in which the game drove the port ends the history, with
 * the same quiet period, because an exchange is several writes and the
 * answers come back over the following frames. */
static uint32_t rw_seen_serial_gen;
static unsigned rw_barriers_serial, rw_serial_frames, rw_w_serial_frames;
static uint32_t rw_w_deferred;
static uint32_t rw_floor_frame;
static int      rw_floor_why;
static unsigned rw_ro_hits_seen, rw_replay_save_hits, rw_rlog_miss_seen;
static int      rw_limit_said;
/* Frames in which the game wrote the save, and frames the history was shut
 * because of it (those plus the quiet frames after): per window in
 * rewind_win, so a game that writes its save all the time shows up. */
static unsigned rw_save_write_frames;
static unsigned rw_w_save_write_frames, rw_w_bk_busy_frames;

/* The history was ended with the machine on a frame play may not continue
 * from (fe_host.h, "ending the history in the middle of a rewind"): the
 * game is reset before its next frame.  Set by fe_host_rewind_suspend, which
 * a power callback may call; acted on by the emulation thread only. */
static volatile int rw_reset_owed;
static int rw_distrust;         /* fe_host_rewind_distrust */
static unsigned rw_lands, rw_land_fails, rw_unlanded_ok, rw_unvetted_resets;
static int rw_gesture_live(void);
static void rw_reset_if_owed(const char *where);

static void rw_barrier(int why)
{
   uint32_t f = fe_rewind_frame_now() + 1;
   fe_rewind_reset(f);
   if (rw_opt.clock_frames)
      gpsp_rewind_clock_sync();     /* nothing reaches back past here */
   rw_floor_frame = f;
   rw_floor_why = why;
   rw_bk_carry = 1;
   if (rw_hash_fp)
      fprintf(rw_hash_fp, "B %u\n", (unsigned)f);
}
/* The core's written-page map as it stood when the snapshot was gathered
 * (1 = written), 1 KiB pages.  A copy, because the core's map starts
 * collecting for the NEXT snapshot the moment this one is gathered. */
static uint8_t  rw_ewram_map[256], rw_vram_map[96], rw_aud_map[64];
static int      rw_pieces;           /* the in-place layout is in use */
static int      rw_maps;             /* rw_opt.dirty bits actually in force */
static uint32_t rw_seen_cheat_gen;
/* ---- a restore that copies only what can differ ---------------------------
 * For every EWRAM / VRAM page: if the machine differs from the reference
 * there, then the core's map has it marked (written since the last
 * snapshot) or rw_diff_* has (a step back changed the reference there and
 * the machine has not followed).  A restore copies exactly those pages and
 * then both are empty; so is rw_diff_* after any snapshot, which leaves
 * machine and reference equal.  rewind_verify checks it after every
 * restore. */
static uint8_t  rw_diff_ewram[256], rw_diff_vram[96];
static uint32_t rw_off_ewram, rw_off_vram;   /* where they lie in the image */
static uint32_t rw_restore_bad;
static uint64_t rw_restore_copied;
static uint32_t rw_restore_kept, rw_restore_n;
static uint64_t rw_restore_arr_us;   /* of a restore's time: the arrays */
static uint64_t rw_now(void);
/* Test instrument: the cartridge save as it is in memory. */
static uint32_t rw_sav_crc(void)
{
   const void *sp = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
   size_t sn = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
   return sp && sn ? fe_crc32(0, sp, sn) : 0;
}
static uint32_t rw_nopic_shown;      /* frames gone back over with no picture */

static void rw_diff_mark(uint8_t *map, uint32_t pages, uint32_t base,
                         uint32_t off, uint32_t bytes)
{
   uint32_t lo, hi, p;
   if (off + bytes <= base || off >= base + (pages << 10))
      return;
   lo = off > base ? off - base : 0;
   hi = off + bytes - base;
   if (hi > pages << 10)
      hi = pages << 10;
   for (p = lo >> 10; p <= (hi - 1) >> 10; p++)
      map[p] = 1;
}

static void rw_ref_touched(uint32_t off, uint32_t bytes)
{
   rw_diff_mark(rw_diff_ewram, 256, rw_off_ewram, off, bytes);
   rw_diff_mark(rw_diff_vram, 96, rw_off_vram, off, bytes);
}

static void rw_snapshot_done(void)
{
   memset(rw_diff_ewram, 0, sizeof(rw_diff_ewram));
   memset(rw_diff_vram, 0, sizeof(rw_diff_vram));
}

static int rw_run_nopic, rw_play_ext;
extern uint32_t sound_synth_off;  /* sound.h */
static int      rw_held_tick;     /* this tick's re-runs are a held rewind's */
static int      rw_inexact;       /* a re-run since the last restore skipped
                                   * the sound synthesis */
static int      rw_cad_quiet;
static uint32_t rw_e_frame, rw_e_pic, rw_e_head;   /* running means, us */
static uint32_t rw_synth_skipped;
/* restore, split (gather_prof): closing the JIT accounting, the verify
 * compare, the maps, the sound in flight */
static uint64_t rw_rs_jit_us, rw_rs_verify_us, rw_rs_maps_us, rw_rs_aud_us;

static int      rw_head_skip = 1; /* fe_host_rewind_set_head_skip */
static uint32_t rw_ring_asked, rw_ring_granted;
static const char *const rw_why_name[FE_RW_LIMIT_N] =
   { "history", "save", "state_load", "cheats", "other", "serial", "sleep",
     "poke", "not_recorded" };
static uint32_t rw_head_skipped;

static void rw_ema(uint32_t *e, uint32_t d)
{
   *e = *e ? (*e * 7 + d) / 8 : d;
}
/* The clock follows the input log's lifetime, including wrap and forks. */
static uint64_t rw_frame_context(void)
{
   return (uint64_t)gpsp_rewind_clock_time(NULL);
}

static void rw_replay_context(uint64_t value)
{
   long long second = (long long)value;
   (void)gpsp_rewind_clock_time(&second);
}
static uint16_t *rw_play_ext_pix;
static uint32_t rw_frame_core_us;    /* retro_run's time for the last frame */
static uint32_t rw_snap_est_us = 3000;
static uint32_t rw_snap_seen;

static int rw_gather(void)
{
   uint32_t used, hi, pg;
   if (rw_pos_cur)
   {
      rw_pos_cur[0] = gba_rewind_rlog_pos;
      rw_pos_cur[1] = 0;
   }
   if (!rw_pieces)
      return gpsp_rewind_gather(rw_scratch, gpsp_rewind_state_size()) ? 0 : -1;
   gpsp_rewind_rest_gather(rw_scratch, rw_rest);
   /* The sound in flight: a kilobyte or two of a 32 KiB span.  Only the
    * pages this save or the previous one reached can differ. */
   used = gpsp_rewind_audio_gather(rw_aud, rw_aud_used);
   hi = used > rw_aud_used ? used : rw_aud_used;
   rw_aud_used = used;
   memset(rw_aud_map, 0, sizeof(rw_aud_map));
   for (pg = 0; pg < sizeof(rw_aud_map) && (pg << RW_AUD_PAGE) < hi; pg++)
      rw_aud_map[pg] = 1;
   if (rw_maps && !gpsp_rewind_take_dirty(rw_ewram_map, rw_vram_map))
   {
      /* The core withdrew the map (gba_rewind_map_ok): read everything. */
      memset(rw_ewram_map, 1, sizeof(rw_ewram_map));
      memset(rw_vram_map, 1, sizeof(rw_vram_map));
   }
   return 0;
}

static int rw_restore(const void *image, uint32_t bytes)
{
   int r;
   uint64_t t_rs = rw_opt.gather_prof ? rw_now() : 0;
   rw_inexact = 0;
   rw_jit_close();
   if (rw_opt.gather_prof)
      rw_rs_jit_us += rw_now() - t_rs;
   if (rw_pos_cur)
      /* the frames run from here read the save the first pass read */
      memcpy(&gba_rewind_rlog_pos, (const uint8_t *)image + rw_pos_off, 4);
   if (!rw_pieces)
      r = gpsp_rewind_restore(image, bytes) ? 0 : -1;
   else
   {
      const uint8_t *q = (const uint8_t *)image + rw_rest_bytes;
      uint8_t ew[256], vr[96];
      const uint8_t *pe = NULL, *pv = NULL;
      uint32_t copied = 0;
      uint64_t t_arr;
      int i, keep = 0, fast = rw_opt.fast_restore && gpsp_rewind_light;
      if (fast)
      {
         unsigned p;
         gpsp_rewind_map_peek(ew, vr);
         if (rw_maps & 2)
         {
            for (p = 0; p < 256; p++)
               ew[p] |= rw_diff_ewram[p];
            pe = ew;
         }
         if (rw_maps & 1)
         {
            for (p = 0; p < 96; p++)
               vr[p] |= rw_diff_vram[p];
            pv = vr;
         }
      }
      t_arr = rw_now();
      keep = !gpsp_rewind_arrays_restore(q, pe, pv, &copied) && fast;
      rw_restore_arr_us += rw_now() - t_arr;
      t_rs = rw_opt.gather_prof ? rw_now() : 0;
      if (rw_opt.verify)
      {
         /* The whole machine must now be the snapshot. */
         const uint8_t *v = q;
         for (i = 0; i < 6; i++)
         {
            if (memcmp(rw_arr[i].live, v, rw_arr[i].bytes) != 0)
            {
               rw_restore_bad++;
               fe_evt("rewind_restore_verify_fail array=%d (a page that "
                      "differed was not copied back)", i);
               memcpy(rw_arr[i].live, v, rw_arr[i].bytes);
               keep = 0;
            }
            v += rw_arr[i].bytes;
         }
      }
      if (rw_opt.gather_prof)
         rw_rs_verify_us += rw_now() - t_rs;
      for (i = 0; i < 6; i++)
         q += rw_arr[i].bytes;
      r = gpsp_rewind_rest_restore(rw_scratch, (const uint8_t *)image, keep)
             ? 0 : -1;
      t_rs = rw_opt.gather_prof ? rw_now() : 0;
      /* Machine == reference: nothing is written, nothing is behind. */
      gpsp_rewind_map_clean();
      rw_snapshot_done();
      if (!fast)
         gpsp_rewind_mark_all();      /* (no map in use: stay conservative) */
      rw_restore_copied += copied;
      rw_restore_kept += (uint32_t)keep;
      rw_restore_n++;
      if (rw_opt.gather_prof)
      {
         uint64_t t = rw_now();
         rw_rs_maps_us += t - t_rs;
         t_rs = t;
      }
      gpsp_rewind_audio_restore(q);
      if (rw_opt.gather_prof)
         rw_rs_aud_us += rw_now() - t_rs;
      /* The reference's sound span may now reach further than the last
       * save did: compare all of it once. */
      rw_aud_used = rw_aud_bytes;
   }
   if (gpsp_rewind_jit_used)
   {
      gpsp_rewind_jit_used(&rw_jit_rom0, &rw_jit_ram0);
      rw_jit_open = 1;
   }
   return r;
}

static void rw_run(uint32_t input, uint16_t *pix, int16_t *aud,
                   uint32_t *aud_n)
{
   int ext = 0, skip = 0, fskip0 = -1, fskip = 0;
   uint32_t synth0 = sound_synth_off;
   uint64_t t_run;
   if (rw_opt.no_capture)
      pix = NULL;
   if (rw_opt.pix_begin)
      ext = rw_opt.pix_begin(pix);
   if (rw_opt.sound_mode == 2 && rw_held_tick)
   {
      /* Nobody hears these frames and the machine goes back to a snapshot
       * before play continues: leave our own mixing out. */
      skip = 1;
      rw_inexact = 1;
      rw_synth_skipped++;
      sound_synth_off = 1;
      fe_rewind_taint();
   }
   rw_replay    = 1;
   rw_cap_pix   = ext ? NULL : pix;
   rw_cap_aud   = (rw_opt.no_capture || rw_opt.sound_mode) ? NULL : aud;
   rw_cap_aud_n = 0;
   rw_cap_got   = 0;
   rw_mask      = input;
   rw_mask_on   = 1;
   gba_rewind_backup_ro = 1;
   if (!pix && rw_head_skip && &frontend_skip_override)
   {
      /* Nobody will look at this frame: the core need not draw or capture
       * it (what a skipped fast-forward frame is). */
      fskip = 1;
      fskip0 = frontend_skip_override;
      frontend_skip_override = 1;
      rw_head_skipped++;
   }
   t_run = rw_now();
   retro_run();
   rw_ema(pix ? &rw_e_pic : &rw_e_head, (uint32_t)(rw_now() - t_run));
   if (fskip)
      frontend_skip_override = fskip0;
   gba_rewind_backup_ro = 0;
   rw_mask_on = 0;
   rw_replay = 0;
   if (skip)
      sound_synth_off = synth0;
   if (pix && !(ext ? (rw_opt.pix_end && rw_opt.pix_end()) : rw_cap_got))
      rw_run_nopic = 1;
   else
      rw_run_nopic = 0;
   if (rw_hash_fp && rw_hash_state && !rw_inexact &&
       gpsp_rewind_gather(rw_hash_state, gpsp_rewind_state_size()))
      /* Test instrument: the machine a RE-RUN frame leaves, to be compared
       * with what the first pass left at that frame (rewind_check.py, Q).
       * (Not for frames run without the sound synthesis: those are known
       * to differ in the sound buffer, and never become play.) */
      fprintf(rw_hash_fp, "Q %u %08x %08x\n", (unsigned)fe_rewind_frame_mach() + 1,
              (unsigned)fe_crc32(0, rw_hash_state, gpsp_rewind_state_size()),
              (unsigned)rw_sav_crc());
   if (rw_hash_fp && rw_hash_state && rw_opt.dump_frame &&
       fe_rewind_frame_mach() + 1 == rw_opt.dump_frame &&
       gpsp_rewind_gather(rw_hash_state, gpsp_rewind_state_size()))
   {
      /* Test instrument: the same frame's image when it is RE-RUN for a
       * rewind, state_<n>_r<k>.bin (see rw_after_frame). */
      static unsigned dump_r;
      char path[600];
      FILE *df;
      snprintf(path, sizeof(path), "%s/state_%u_r%u.bin",
               rw_opt.dump_dir ? rw_opt.dump_dir : ".",
               (unsigned)rw_opt.dump_frame, dump_r++);
      df = dump_r <= 4 ? fopen(path, "wb") : NULL;
      if (df)
      {
         fwrite(rw_hash_state, 1, gpsp_rewind_state_size(), df);
         fclose(df);
      }
   }
   if (aud_n)
      *aud_n = (rw_cap_aud ? rw_cap_aud_n : 0) |
               (rw_run_nopic ? FE_RW_NOPIC : 0);
   if (pix && !rw_run_nopic && !ext)
      rw_cap_last = pix;
}

static uint64_t rw_now(void)
{
   return host.time_us ? host.time_us() : 0;
}

/* A state was loaded: the machine is somewhere else entirely.  The history
 * restarts, and so does the watch on the save counter (a state load does
 * not write the save, but whatever the counter did before it is not this
 * timeline's business). */
static void fe_host_rewind_state_loaded(void)
{
   rw_seen_gen = sram_gen_now();
   rw_seen_serial_gen = gba_rewind_serial_gen;
   rw_quiet = 0;
   rw_bk_quiet = 0xFFFF;        /* another machine: its chip is as loaded */
   rw_wr_quiet = 0xFFFF;
   rw_barrier(FE_RW_LIMIT_STATE);
}

const char *fe_host_rewind_blocked(void)
{
   if (!rw_on)
      return "off";
   if (gb_family)
      return "gb";              /* GB/GBC: not built in this prototype */
   /* Every kind of link SESSION: lockstep partners and the emulated
    * wireless adapter cannot be taken back in time.  A session, not the
    * ability to have one: `gba_cable` is set at every GBA game load in a
    * build that can host a cable room (a player's build always), and
    * testing it here refused the rewind in every game.  A running session
    * is `gbl` (GB and GBA cable) or the pair being active. */
   if (gbl)
      return "link";
#ifdef GBA_LINK
   if (fe_gbadual_active())
      return "link";
#endif
   if (fe_np_session_active())
      return "link";
   return NULL;
}

void fe_host_rewind_set_ff(int ff)
{
   rw_ff = ff;
}

int fe_host_rewind_replaying(void)
{
   return rw_replay;
}

static uint32_t rw_crc_rev(const int16_t *a, uint32_t n)
{
   /* CRC of the stereo frames in reverse order: what a rewind plays. */
   uint32_t crc = 0;
   while (n--)
      crc = fe_crc32(crc, a + n * 2, 4);
   return crc;
}

/* Test instrument: buttons as a pure function of the timeline frame, so a
 * run that rewinds and a run that never does play the same game. */
static uint32_t rw_fuzz_mask(uint32_t seed, uint32_t frame)
{
   static const uint8_t dir[5] = { 0, RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_RIGHT };
   uint32_t h = (frame / 12u) * 2654435761u ^ seed * 40503u;
   uint32_t m = 0;
   h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
   if ((h & 3) != 0)        m |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
   if (((h >> 2) & 7) == 0) m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
   if (((h >> 5) & 15) == 0) m |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
   if (((h >> 9) & 15) == 0) m |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
   if (dir[(h >> 13) % 5])  m |= 1u << dir[(h >> 13) % 5];
   return m;
}

/* The input the next normal frame runs with, and where its picture goes. */
static void rw_latch_input(void)
{
   uint32_t f;
   if (rw_reset_owed)
      rw_reset_if_owed("frame");   /* (the platform resumed nothing) */
   if (!rw_on)
      return;
   f = fe_rewind_frame_now() + 1;
   rw_mask = (host.input_bitmask ? host.input_bitmask() : 0) | injected_mask;
   if (rw_opt.fuzz_from && f >= rw_opt.fuzz_from)
      rw_mask = rw_fuzz_mask(rw_opt.fuzz_seed, f);
   else if (rw_opt.redo_inputs && f <= rw_redo_high)
      rw_mask = fe_rewind_logged_input(f);
   rw_mask_on = 1;
   rw_test_aud_n = 0;
   rw_cap_aud_n = 0;
   rw_cap_got = 0;
   rw_play_ext = 0;
   rw_tee = fe_rewind_play_slot(&rw_cap_pix, &rw_cap_aud);
   /* play_slot may gather the preceding frame. Sample only afterwards so
    * that snapshot retains its own time, and this frame gets wall time. */
   if (rw_opt.clock_frames)
      gpsp_rewind_clock_sync();
   if (rw_opt.no_capture)
      rw_tee = 0;               /* the slot exists (1 pixel); nothing goes in */
   if (rw_opt.pix_begin)
   {
      rw_play_ext = rw_opt.pix_begin(rw_tee ? rw_cap_pix : NULL);
      if (rw_play_ext)
         rw_play_ext_pix = rw_cap_pix, rw_cap_pix = NULL;   /* sound only */
   }
}

/* After every normally-run GBA frame. */
static void rw_after_frame(void)
{
   uint32_t gen, bk_flags = 0;
   int tee = rw_tee;

   int has_pic = 0, hash_ok;

   rw_mask_on = 0;              /* the latch is for one retro_run only */
   rw_tee = 0;
   if (!rw_on)
      return;
   if (tee)
   {
      if (rw_play_ext)
         has_pic = rw_opt.pix_end && rw_opt.pix_end();
      else
      {
         has_pic = rw_cap_got;
         if (has_pic)
            rw_cap_last = rw_cap_pix;
      }
   }
   else if (rw_play_ext && rw_opt.pix_end)
      (void)rw_opt.pix_end();
   gen = sram_gen_now();
   {
      /* The save chip in this frame (see "the save is permanent"). */
      int busy = gba_rewind_bk_acc || gba_rewind_backup_busy() ||
                 gen != rw_seen_gen;
      gba_rewind_bk_acc = 0;
      if (busy)
      {
         if (rw_bk_quiet && rw_bk_quiet < 0xFFFF)
         {
            static const uint16_t lim[7] = { 4, 8, 15, 30, 60, 120, 300 };
            unsigned b = 0;
            while (b < 7 && rw_bk_quiet > lim[b])
               b++;
            rw_bk_gaps[b]++;
         }
         rw_bk_quiet = 0;
         rw_bk_busy_frames++;
         bk_flags |= FE_RW_IN_BUSY;
      }
      else if (rw_bk_quiet < 0xFFFF)
         rw_bk_quiet++;
      if (gen != rw_seen_gen)
      {
         rw_save_write_frames++;
         bk_flags |= FE_RW_IN_CHANGE;
      }
      if (gen != rw_seen_gen || gba_rewind_backup_busy())
         rw_wr_quiet = 0;
      else if (rw_wr_quiet < 0xFFFF)
         rw_wr_quiet++;
   }
   if (rw_poked)
   {
      rw_poked = 0;
      rw_seen_gen = gen;
      rw_barrier(FE_RW_LIMIT_POKE);
      return;
   }
   if (gba_rewind_cheat_gen != rw_seen_cheat_gen)
   {
      /* The cheat list changed (a code switched on or off, the list
       * reloaded, cheats suspended).  It is not part of a snapshot, and a
       * frame re-run under another list is another frame: the history ends
       * here, like at a cartridge save. */
      rw_seen_cheat_gen = gba_rewind_cheat_gen;
      rw_seen_gen = gen;
      rw_barriers_cheat++;
      rw_barrier(FE_RW_LIMIT_CHEAT);
      return;
   }
   if (fe_host_rewind_blocked() || (rw_ff && !rw_opt.ff_rewindable))
   {
      /* Not recorded: the history restarts when this ends. */
      rw_seen_gen = gen;
      rw_barrier(FE_RW_LIMIT_OFF);
      return;
   }
   rw_seen_gen = gen;
   if (rw_rlog && gba_rewind_rlog_pos != rw_rlog_pos_seen)
   {
      /* The game read its save: the log moved.  History whose reads have
       * left the log's ring cannot be re-run faithfully and is dropped. */
      uint32_t cb = rw_rlog_bytes / RW_RLOG_CHUNKS, c, k;
      uint32_t f = fe_rewind_frame_now() + 1;
      rw_rlog_pos_seen = gba_rewind_rlog_pos;
      c = (gba_rewind_rlog_pos - 1) / cb;
      if (c != rw_rlog_chunk)
      {
         uint32_t need;
         for (k = c - rw_rlog_chunk < RW_RLOG_CHUNKS
                     ? rw_rlog_chunk + 1 : c - (RW_RLOG_CHUNKS - 1);
              k != c + 1; k++)
            rw_rlog_cframe[k % RW_RLOG_CHUNKS] = f;
         rw_rlog_chunk = c;
         /* all chunks but the oldest are whole in the ring */
         need = rw_rlog_cframe[(c - (RW_RLOG_CHUNKS - 2)) % RW_RLOG_CHUNKS];
         if (need)
         {
            uint32_t old0 = fe_rewind_frame_oldest();
            if (need >= f || fe_rewind_trim(need) < need)
            {
               /* (this one frame read more than the log holds, or only the
                * newest snapshot is left and it is too old) */
               rw_rlog_resets++;
               rw_barrier(FE_RW_LIMIT_SAVE);
               return;
            }
            if (fe_rewind_frame_oldest() != old0)
            {
               rw_rlog_trims++;
               rw_floor_frame = need;
               rw_floor_why = FE_RW_LIMIT_SAVE;
            }
         }
      }
   }
   if (gba_rewind_serial_gen != rw_seen_serial_gen)
   {
      /* The game drove the serial port in this frame. */
      rw_seen_serial_gen = gba_rewind_serial_gen;
      rw_serial_frames++;
      if (!rw_quiet)
      {
         rw_barriers_serial++;
         rw_quiet_why = FE_RW_LIMIT_SERIAL;
      }
      rw_quiet = rw_opt.save_quiet;
      rw_barrier(rw_quiet_why);
      return;
   }
   if (rw_quiet)
   {
      /* Between the writes of a serial exchange, or just after the last
       * one. */
      rw_quiet--;
      rw_barrier(rw_quiet_why);
      return;
   }
   /* Test instrument: the image hashed for this frame is taken BEFORE a
    * snapshot can be (which brings the clock's base up to the wall clock):
    * it is the machine the frame left, which is what a re-run leaves. */
   hash_ok = rw_hash_fp && rw_hash_state &&
             gpsp_rewind_gather(rw_hash_state, gpsp_rewind_state_size());
   if (rw_opt.slack_margin_us)
      fe_rewind_hint_busy(!rw_ff && rw_frame_core_us &&
                          rw_frame_core_us + rw_opt.slack_margin_us +
                          rw_snap_est_us >
                          (uint32_t)(1000000.0 / FE_GBA_FPS));
   if (rw_bk_carry)
   {
      /* The first frame of a history: what the chip was doing just before
       * it is not in the log of frames. */
      rw_bk_carry = 0;
      if (rw_bk_quiet < rw_opt.save_quiet)
         bk_flags |= FE_RW_IN_BUSY;
   }
   fe_rewind_frame((rw_mask & FE_RW_IN_KEYS) | bk_flags,
                   (tee ? rw_cap_aud_n : 0) |
                   (tee && !has_pic ? FE_RW_NOPIC : 0));
   {
      /* What a snapshot costs here, for the busy test: the last one, or a
       * slowly forgotten larger one. */
      const fe_rewind_stats *t = fe_rewind_get_stats();
      static uint64_t us_seen;
      if (t->snapshots != rw_snap_seen)
      {
         uint32_t last = (uint32_t)(t->gather_us + t->pack_us - us_seen);
         rw_snap_est_us -= rw_snap_est_us / 8;
         if (last > rw_snap_est_us)
            rw_snap_est_us = last;
         rw_snap_seen = t->snapshots;
      }
      us_seen = t->gather_us + t->pack_us;
   }
   if (fe_rewind_frame_now() > rw_redo_high)
      rw_redo_high = fe_rewind_frame_now();
   if (rw_hash_fp)
   {
      uint32_t sc = 0, vc = 0, bc;
      char vtxt[12];
      if (hash_ok)
      {
         /* Test instrument: RW_DUMP_FRAME=n writes the image hashed for
          * timeline frame n, every time that frame is run, to
          * RW_DUMP_DIR/state_<n>_<k>.bin -- to see WHICH bytes differ when
          * rewind_check.py says a state does. */
         static long dump_frame = -2;
         static unsigned dump_k;
         sc = fe_crc32(0, rw_hash_state, gpsp_rewind_state_size());
         if (dump_frame == -2)
            dump_frame = rw_opt.dump_frame ? (long)rw_opt.dump_frame :
               getenv("RW_DUMP_FRAME") ? atol(getenv("RW_DUMP_FRAME")) : -1;
         if (dump_frame >= 0 && (long)fe_rewind_frame_now() == dump_frame)
         {
            char path[600];
            FILE *df;
            snprintf(path, sizeof(path), "%s/state_%ld_%u.bin",
                     rw_opt.dump_dir ? rw_opt.dump_dir :
                     getenv("RW_DUMP_DIR") ? getenv("RW_DUMP_DIR") : ".",
                     dump_frame, dump_k++);
            df = fopen(path, "wb");
            if (df)
            {
               fwrite(rw_hash_state, 1, gpsp_rewind_state_size(), df);
               fclose(df);
            }
            /* ...and its picture, 240x160 RGB565, as pix_<n>_<k>.bin */
            snprintf(path, sizeof(path), "%s/pix_%ld_%u.bin",
                     rw_opt.dump_dir ? rw_opt.dump_dir :
                     getenv("RW_DUMP_DIR") ? getenv("RW_DUMP_DIR") : ".",
                     dump_frame, dump_k - 1);
            df = last_frame ? fopen(path, "wb") : NULL;
            if (df)
            {
               unsigned y;
               for (y = 0; y < FE_GBA_HEIGHT; y++)
                  fwrite((const uint8_t *)last_frame + y * last_pitch, 1,
                         FE_GBA_WIDTH * 2, df);
               fclose(df);
            }
         }
      }
      /* The picture hashed is the one the rewind KEEPS for this frame (what
       * it will show when the frame is gone back over).  On a platform
       * whose pictures come from elsewhere than the core (the PSP's second
       * core: pix_begin / pix_end) the core's own buffer is not drawn at
       * all, and hashing it compared every backward picture with something
       * nobody saw (1388 of 1388 "wrong" on a PSP-3000).  A frame with no
       * kept picture has no hash there ("--------"); the checker says how
       * many. */
      {
         const uint16_t *hp = (tee && has_pic)
            ? (rw_play_ext ? rw_play_ext_pix : rw_cap_pix) : NULL;
         unsigned y;
         if (hp)
         {
            for (y = 0; y < FE_GBA_HEIGHT; y++)
               vc = fe_crc32(vc, hp + y * rw_pitch, FE_GBA_WIDTH * 2);
            snprintf(vtxt, sizeof(vtxt), "%08x", (unsigned)vc);
         }
         else if (rw_opt.pix_begin)
            snprintf(vtxt, sizeof(vtxt), "--------");
         else
         {
            if (last_frame)
               for (y = 0; y < FE_GBA_HEIGHT; y++)
                  vc = fe_crc32(vc, (const uint8_t *)last_frame + y * last_pitch,
                                FE_GBA_WIDTH * 2);
            snprintf(vtxt, sizeof(vtxt), "%08x", (unsigned)vc);
         }
      }
      /* ...and the cartridge save as it is in memory (a rewind never
       * changes it), how many of its bytes the game has read so far, and
       * whether this frame is one the chip was busy in. */
      bc = rw_sav_crc();
      fprintf(rw_hash_fp, "F %u %08x %s %08x %u %04x %08x %u %u\n",
              (unsigned)fe_rewind_frame_now(), (unsigned)sc, vtxt,
              (unsigned)rw_crc_rev(rw_test_aud, rw_test_aud_n),
              (unsigned)rw_test_aud_n, (unsigned)(rw_mask & FE_RW_IN_KEYS),
              (unsigned)bc, (unsigned)gba_rewind_rlog_end,
              (unsigned)((bk_flags & FE_RW_IN_BUSY) != 0));
   }
}

/* The options, with the defaults and the limits applied, as a core config
 * (its spans are set by the caller). */
static void rw_config(const fe_host_rewind_opts *o, fe_host_rewind_opts *out,
                      fe_rewind_config *c)
{
   *out = *o;
   if (!out->ring_bytes)
   {
      /* 1..8 MiB (docs/REWIND.md): 1 so a PSP-1000 can take part, 8
       * because past that the ring is no longer what limits a rewind.
       * The PSP's Settings offer 1, 2 and 4 and always name one; the 2
       * below is only what a caller that names NO size gets (the desktop
       * twin and its records). */
      uint32_t mib = out->ring_mib ? out->ring_mib : 2;
      if (mib < 1) mib = 1;
      if (mib > 8) mib = 8;
      out->ring_mib = mib;
      out->ring_bytes = mib << 20;
   }
   if (!out->interval)   out->interval   = 6;
   if (!out->save_quiet) out->save_quiet = 60;
   {
      /* the read log: a power of two, 16 KiB .. 1 MiB */
      uint32_t b = 16384u;
      while (b < (out->rlog_bytes ? out->rlog_bytes : 131072u) && b < (1u << 20))
         b <<= 1;
      out->rlog_bytes = b;
      rw_rlog_bytes = b;
   }
   if (!out->max_frames) out->max_frames = 16384;

   memset(c, 0, sizeof(*c));
   c->ring_bytes   = out->ring_bytes;
   c->interval     = out->interval;
   if (!out->pix_pitch) out->pix_pitch = FE_GBA_WIDTH;
   if (!out->pix_rows)  out->pix_rows  = FE_GBA_HEIGHT;
   if (out->slack_margin_us && !out->max_span)
      out->max_span = 8 * out->interval;
   c->pace_us      = out->pace_us;
   c->max_span     = out->max_span;
   c->max_frames   = out->max_frames;
   c->frame_pixels = out->no_capture ? 1 : out->pix_pitch * out->pix_rows;
   c->ref_touched  = rw_ref_touched;
   c->snapshot_done = rw_snapshot_done;
   c->audio_cap    = out->no_capture ? 1 : RW_AUD_CAP;
   c->burst        = out->burst;
   c->tick_budget_us = out->tick_budget_us;
   c->verify       = out->verify;
   c->gather       = rw_gather;
   c->restore      = rw_restore;
   c->run          = rw_run;
   if (out->clock_frames)
   {
      c->frame_context = rw_frame_context;
      c->replay_context = rw_replay_context;
   }
   c->now_us       = host.time_us ? rw_now : NULL;
}

static size_t rw_mem_bytes;     /* size of rw_mem */

/* ---- where the memory comes from ------------------------------------------
 * The host's small buffers (rw_buf: the rest of the state, the sound in
 * flight, and in the image layout the image), the core's block (rw_mem: the
 * reference image, the logs, the pictures) and the delta ring.
 *
 * Without a pool: three heap blocks.  With one (fe_host_rewind_set_pool)
 * rw_buf and rw_mem are carved from its END downwards and the ring takes
 * what is left of the pool; only the part of the ring that does not fit
 * goes to the heap (the ring may be in two pieces, fe_rewind_config).  So
 * the heap pays exactly for the megabytes of history beyond what the pool
 * holds, and nothing when the pool holds it all.
 *
 * The in-place layout needs a save-state image as scratch (the writers are
 * run into it and the few KiB between the arrays gathered from it).  That is
 * state_buf, the static buffer every state save and load already goes
 * through: it costs nothing, and anything else that writes it leaves a
 * valid image of the same layout behind.
 *
 * Sizes are those of the image layout, the larger of the two, so a reserve
 * before the game is loaded and the enable after it agree.
 *
 * Nothing in a pool survives fe_host_rewind_suspend: no pointer into it is
 * kept, and resume builds everything again from the options. */
static uint8_t *rw_pool_base, *rw_pool_top, *rw_pool_low;
static int      rw_buf_pool, rw_mem_pool;   /* the block is pool memory */
static uint8_t *rw_ring_pool;               /* the ring's piece in the pool */
static size_t   rw_ring_pool_bytes;
static uint8_t *rw_ring_heap;               /* its piece (or all) on the heap */
static size_t   rw_ring_heap_bytes;
static int      rw_suspended;               /* rw_opt is to be resumed */
static uint32_t rw_resume_frame;            /* the timeline goes on from here */

void fe_host_rewind_set_pool(void *base, size_t bytes)
{
   if (rw_on)
      return;                   /* only between suspend and resume, or before */
   rw_pool_base = (uint8_t *)base;
   rw_pool_top  = base ? (uint8_t *)base + (bytes & ~(size_t)63) : NULL;
   rw_pool_low  = rw_pool_top;
}

void *fe_host_rewind_pool_low(void)
{
   return rw_pool_low;
}

/* n bytes from the end of the pool, 64-byte aligned; NULL = no room. */
static uint8_t *rw_pool_take(size_t n)
{
   n = (n + 63u) & ~(size_t)63u;
   if (!rw_pool_base || (size_t)(rw_pool_low - rw_pool_base) < n)
      return NULL;
   rw_pool_low -= n;
   return rw_pool_low;
}

static size_t rw_buf_need(int inplace)
{
   return RW_REST_CAP + gpsp_rewind_audio_bytes() +
          (inplace ? 0 : gpsp_rewind_state_size() + 8) + rw_rlog_bytes;
}

/* Everything, once: a second call finds it there.  cu: the configuration
 * with the image layout's spans (the size to plan with).  On return
 * *ring_bytes is the ring actually obtained (less than asked only if the
 * heap refused its part).  0 = ok. */
static int rw_mem_take(const fe_rewind_config *cu, int inplace,
                       uint32_t *ring_bytes)
{
   fe_rewind_config t = *cu;
   size_t fixed, buf = rw_buf_need(inplace);
   uint32_t ring = cu->ring_bytes, a = 0, b;

   if (rw_mem)
   {
      *ring_bytes = (uint32_t)(rw_ring_pool_bytes + rw_ring_heap_bytes);
      return 0;
   }
   t.ring_mem = (void *)1;                    /* the size without the ring */
   fixed = fe_rewind_mem_bytes(&t);
   if (!fixed)
      return -1;
   rw_ring_pool = NULL;
   rw_ring_pool_bytes = 0;

   if (rw_pool_base && (size_t)(rw_pool_low - rw_pool_base) >=
                       ((buf + 63u) & ~(size_t)63u) + ((fixed + 63u) & ~(size_t)63u))
   {
      size_t room;
      rw_buf = rw_pool_take(buf);
      rw_mem = rw_pool_take(fixed);
      rw_buf_pool = rw_mem_pool = 1;
      room = (size_t)(rw_pool_low - rw_pool_base) & ~(size_t)0xFFFF;
      a = ring < room ? ring : (uint32_t)room;
      b = ring - a;
      if (rw_ring_heap && rw_ring_heap_bytes != b)
      {
         free(rw_ring_heap);                  /* (kept across a suspend) */
         rw_ring_heap = NULL;
         rw_ring_heap_bytes = 0;
      }
      if (b && !rw_ring_heap)
      {
         /* What the heap will not give is asked for again half a megabyte
          * smaller, down to nothing: it used to be all or nothing, so a
          * size the heap could not hold in full gave LESS history than a
          * smaller setting (the pool's piece alone). */
         while (b && !(rw_ring_heap = (uint8_t *)malloc(b)))
            b = b > 524288u ? b - 524288u : 0;
         rw_ring_heap_bytes = rw_ring_heap ? b : 0;
      }
      if (a)
      {
         rw_ring_pool = rw_pool_take(a);
         rw_ring_pool_bytes = a;
      }
      if (!rw_ring_pool_bytes && !rw_ring_heap_bytes)
      {
         rw_buf = NULL;                       /* pool memory: nothing to free */
         rw_mem = NULL;
         rw_buf_pool = rw_mem_pool = 0;
         return -1;
      }
   }
   else
   {
      rw_buf_pool = rw_mem_pool = 0;
      rw_buf = (uint8_t *)malloc(buf);
      rw_mem = malloc(fixed);
      if (rw_ring_heap && rw_ring_heap_bytes != ring)
      {
         free(rw_ring_heap);
         rw_ring_heap = NULL;
      }
      if (!rw_ring_heap)
         rw_ring_heap = (uint8_t *)malloc(ring);
      rw_ring_heap_bytes = rw_ring_heap ? ring : 0;
      if (!rw_buf || !rw_mem || !rw_ring_heap)
      {
         free(rw_buf);
         free(rw_mem);
         free(rw_ring_heap);
         rw_buf = NULL;
         rw_mem = NULL;
         rw_ring_heap = NULL;
         rw_ring_heap_bytes = 0;
         return -1;
      }
   }
   rw_buf_bytes = buf;
   rw_mem_bytes = fixed;
   *ring_bytes = (uint32_t)(rw_ring_pool_bytes + rw_ring_heap_bytes);
   return 0;
}

static void rw_mem_drop(int keep_heap_ring)
{
   if (rw_mem && !rw_mem_pool)
      free(rw_mem);
   rw_mem = NULL;
   rw_mem_pool = 0;
   rw_mem_bytes = 0;
   if (rw_buf && !rw_buf_pool)
      free(rw_buf);
   rw_buf = NULL;
   rw_buf_pool = 0;
   rw_buf_bytes = 0;
   rw_ring_pool = NULL;
   rw_ring_pool_bytes = 0;
   gba_rewind_rlog = NULL;      /* (it was in rw_buf) */
   rw_rlog = NULL;
   rw_pos_cur = NULL;
   if (!keep_heap_ring)
   {
      free(rw_ring_heap);
      rw_ring_heap = NULL;
      rw_ring_heap_bytes = 0;
   }
   rw_pool_base = rw_pool_top = rw_pool_low = NULL;
}

/* Where the ring is, for a core configuration. */
static void rw_ring_place(fe_rewind_config *c, uint32_t ring_bytes)
{
   c->ring_bytes = ring_bytes;
   c->ring_mem2 = NULL;
   c->ring_split = ring_bytes;
   if (rw_ring_pool_bytes)
   {
      c->ring_mem = rw_ring_pool;
      if (rw_ring_heap_bytes)
      {
         c->ring_mem2 = rw_ring_heap;
         c->ring_split = (uint32_t)rw_ring_pool_bytes;
      }
   }
   else
      c->ring_mem = rw_ring_heap;
}

static void rw_mem_evt(const char *what, int rc)
{
   (void)what; (void)rc;         /* (a build without the event log) */
   fe_evt("%s buf=%u@%s mem=%u@%s ring=%u@pool+%u@heap pool=%08x..%08x "
          "low=%08x %s", what, (unsigned)rw_buf_bytes,
          rw_buf_pool ? "pool" : "heap", (unsigned)rw_mem_bytes,
          rw_mem_pool ? "pool" : "heap", (unsigned)rw_ring_pool_bytes,
          (unsigned)rw_ring_heap_bytes, (unsigned)(uintptr_t)rw_pool_base,
          (unsigned)(uintptr_t)rw_pool_top, (unsigned)(uintptr_t)rw_pool_low,
          rc == 0 ? "ok" : "FAILED");
}

/* One span over the whole serialised image. */
static void rw_spans_image(void)
{
   rw_pieces = 0;
   rw_maps = 0;
   /* (before the buffers exist this only sizes things: cur is never read) */
   rw_scratch = rw_buf ? rw_buf + RW_REST_CAP + gpsp_rewind_audio_bytes()
                       : state_buf;
   rw_spans[0].bytes = (uint32_t)gpsp_rewind_state_size() + 8;
   rw_spans[0].cur = rw_scratch;
   rw_pos_off = (uint32_t)gpsp_rewind_state_size();
   rw_pos_cur = rw_buf ? (uint32_t *)(void *)(rw_scratch + rw_pos_off) : NULL;
   rw_rlog = rw_buf ? rw_scratch + rw_pos_off + 8 : NULL;
   rw_spans[0].dirty = NULL;
   rw_spans[0].page_shift = 0;
   rw_nspans = 1;
}

/* The rest, the six arrays where they lie, the sound.  0 = not possible
 * here (then the image layout is used). */
static int rw_spans_pieces(void)
{
   uint32_t i, n = 0;
   rw_maps      = 0;
   rw_scratch   = state_buf;      /* see "where the memory comes from" */
   rw_rest      = rw_buf;
   rw_aud       = rw_rest + RW_REST_CAP;
   rw_aud_bytes = gpsp_rewind_audio_bytes();
   rw_rest_bytes = gpsp_rewind_layout(rw_scratch, rw_arr);
   /* (the read log's position rides at the end of the rest) */
   rw_pos_off = rw_rest_bytes;
   rw_pos_cur = (uint32_t *)(void *)(rw_rest + rw_pos_off);
   rw_rlog = rw_rest + RW_REST_CAP + rw_aud_bytes;
   if (rw_rest_bytes)
      rw_rest_bytes += 8;
   if (!rw_rest_bytes || rw_rest_bytes > RW_REST_CAP || (rw_pos_off & 3) ||
       (rw_aud_bytes >> RW_AUD_PAGE) >= sizeof(rw_aud_map))
      return 0;
   for (i = 0; i < 6; i++)
      if (((uintptr_t)rw_arr[i].live & 3) || (rw_arr[i].bytes & 3))
         return 0;               /* compared by word: must be word-aligned */
   memset(rw_rest, 0, RW_REST_CAP + rw_aud_bytes);
   memset(rw_spans, 0, sizeof(rw_spans));
   rw_spans[n].bytes = rw_rest_bytes;
   rw_spans[n++].cur = rw_rest;
   rw_off_ewram = rw_rest_bytes + rw_arr[0].bytes;
   rw_off_vram  = rw_off_ewram + rw_arr[1].bytes;
   for (i = 0; i < 6; i++)
   {
      rw_spans[n].bytes = rw_arr[i].bytes;
      rw_spans[n].cur = rw_arr[i].live;
      if (i == 1 && (rw_opt.dirty & 2) &&
          rw_arr[i].bytes == sizeof(rw_ewram_map) << 10)
      {
         rw_spans[n].dirty = rw_ewram_map;     /* EWRAM, 1 KiB pages */
         rw_spans[n].page_shift = 10;
         rw_maps |= 2;
      }
      if (i == 2 && (rw_opt.dirty & 1) &&
          rw_arr[i].bytes == sizeof(rw_vram_map) << 10)
      {
         rw_spans[n].dirty = rw_vram_map;      /* VRAM, 1 KiB pages */
         rw_spans[n].page_shift = 10;
         rw_maps |= 1;
      }
      n++;
   }
   rw_spans[n].bytes = rw_aud_bytes;
   rw_spans[n].cur = rw_aud;
   rw_spans[n].dirty = rw_aud_map;
   rw_spans[n++].page_shift = RW_AUD_PAGE;
   rw_nspans = n;
   rw_aud_used = rw_aud_bytes;   /* nothing known about the buffer yet */
   memset(rw_vram_map, 1, sizeof(rw_vram_map));
   memset(rw_ewram_map, 1, sizeof(rw_ewram_map));
   rw_pieces = 1;
   return 1;
}

void fe_host_rewind_prepare(const fe_host_rewind_opts *o)
{
   gpsp_rewind_marks_want(o && o->inplace && o->dirty);
}

int fe_host_rewind_reserve(const fe_host_rewind_opts *o)
{
   fe_host_rewind_opts opt;
   fe_rewind_config c;
   uint32_t ring = 0;
   int rc;

   if (rw_on || rw_mem)
      return -1;
   fe_host_rewind_prepare(o);
   /* Sized for the image layout, which is the larger of the two. */
   rw_config(o, &opt, &c);
   rw_spans_image();
   c.spans = rw_spans;
   c.nspans = rw_nspans;
   rc = rw_mem_take(&c, opt.inplace, &ring);
   rw_mem_evt("rewind_reserve", rc);
   return rc;
}

int fe_host_rewind_enable(const fe_host_rewind_opts *o)
{
   fe_rewind_config c;
   uint32_t s, state = 0, ring = 0;
   int was_suspended;

   if (rw_on || gb_family)
      return -1;
   rw_config(o, &rw_opt, &c);
   rw_pitch = rw_opt.pix_pitch;
   rw_spans_image();
   c.spans = rw_spans;
   c.nspans = rw_nspans;
   if (rw_mem_take(&c, rw_opt.inplace, &ring) != 0)
   {
      rw_mem_drop(0);
      rw_mem_evt("rewind_enable", -1);
      return -1;
   }
   if (!rw_opt.inplace || !rw_spans_pieces())
   {
      if (rw_opt.inplace)
      {
         /* The in-place layout was asked for and is not possible here, and
          * the buffers were sized without an image: not recording at all is
          * the honest outcome. */
         rw_mem_drop(0);
         fe_evt("rewind_enable FAILED (in-place layout)");
         return -1;
      }
      rw_spans_image();
   }
   c.spans = rw_spans;
   c.nspans = rw_nspans;
   for (s = 0; s < rw_nspans; s++)
      state += rw_spans[s].bytes;
   rw_ring_place(&c, ring);
   rw_opt.ring_bytes = ring;
   rw_ring_asked = rw_opt.ring_mib ? rw_opt.ring_mib * 1048576u : c.ring_bytes;
   if (fe_rewind_init(&c, rw_mem, rw_mem_bytes) == 0)
   {
      fe_rewind_set_target(rw_opt.target_s, rw_opt.gap_max ? rw_opt.gap_max : 24);
      rw_ring_granted = c.ring_bytes;
      fe_evt("rewind_ring asked=%u granted=%u (pool %u + heap %u) target_s=%u",
             (unsigned)rw_ring_asked, (unsigned)c.ring_bytes,
             (unsigned)rw_ring_pool_bytes, (unsigned)rw_ring_heap_bytes,
             (unsigned)rw_opt.target_s);
   }
   else
   {
      rw_mem_drop(0);
      fe_evt("rewind_enable FAILED (init) mem=%u", (unsigned)rw_mem_bytes);
      return -1;
   }
   rw_snapshot_done();
   gpsp_rewind_mark_all();
   gpsp_rewind_clock_mode(rw_opt.clock_frames);
   was_suspended = rw_suspended;
   rw_suspended = 0;
   if (rw_opt.hash_path && !rw_hash_fp)
   {
      rw_hash_fp = fopen(rw_opt.hash_path, "w");
      rw_hash_state = (uint8_t *)malloc(gpsp_rewind_state_size());
   }
   rw_on = 1;
   gpsp_rewind_light = rw_opt.full_flush_restore ? 0 : 1;
   rw_seen_gen = sram_gen_now();
   rw_seen_cheat_gen = gba_rewind_cheat_gen;
   rw_seen_serial_gen = gba_rewind_serial_gen;
   rw_quiet = 0;
   rw_poked = 0;
   rw_ro_hits_seen = gba_rewind_backup_ro_hits;
   /* The read log starts empty: nothing before this frame is re-run. */
   gba_rewind_rlog_mask = rw_rlog_bytes - 1;
   gba_rewind_rlog_pos = gba_rewind_rlog_end = rw_rlog_pos_seen = 0;
   gba_rewind_bk_acc = 0;
   gba_rewind_rlog_mutant = (uint32_t)rw_opt.sav_mutant & 3u;
   memset(rw_rlog_cframe, 0, sizeof(rw_rlog_cframe));
   rw_rlog_chunk = 0;
   gba_rewind_rlog = rw_rlog;
   rw_bk_carry = 1;
   fe_rewind_set_noland((rw_opt.sav_mutant & 4) ? 0 : rw_opt.save_quiet);
   if (rw_opt.gather_prof)
      gpsp_rewind_gprof(NULL, rw_now);
   /* The timeline starts at the frame count; after a suspend it goes on
    * from the frame the machine was left at (the two differ once a rewind
    * has gone back). */
   fe_rewind_reset(was_suspended ? rw_resume_frame : frame_count);
   rw_floor_frame = fe_rewind_frame_now();
   rw_floor_why = was_suspended ? FE_RW_LIMIT_SLEEP : FE_RW_LIMIT_HISTORY;
   if (was_suspended && rw_hash_fp)
      fprintf(rw_hash_fp, "W %u\n", (unsigned)fe_rewind_frame_now());
   rw_win_mark();
   rw_mem_evt("rewind_mem", 0);
   fe_evt("rewind_enable mem=%u+%u state=%u ring=%u interval=%u max_frames=%u "
          "capture=%u burst=%u save_quiet=%u ff_rewindable=%d light=%d "
          "inplace=%d dirty=%d pace_us=%u verify=%d fast_restore=%d "
          "clock_frames=%d slack_us=%u max_span=%u marks=%d ext_pix=%d",
          (unsigned)rw_mem_bytes, (unsigned)rw_buf_bytes, (unsigned)state,
          (unsigned)c.ring_bytes,
          (unsigned)c.interval, (unsigned)c.max_frames,
          rw_opt.no_capture ? 0u :
          (unsigned)(2u * c.interval * (c.frame_pixels * 2 + c.audio_cap * 4)),
          (unsigned)(c.burst ? c.burst : 2),
          (unsigned)rw_opt.save_quiet, rw_opt.ff_rewindable,
          !rw_opt.full_flush_restore, rw_pieces, rw_maps,
          (unsigned)c.pace_us, rw_opt.verify, rw_opt.fast_restore,
          rw_opt.clock_frames, (unsigned)rw_opt.slack_margin_us,
          (unsigned)c.max_span, gpsp_rewind_marks_ok(),
          rw_opt.pix_begin != NULL);
   return 0;
}

void fe_host_rewind_suspend(void)
{
   if (!rw_on)
      return;
   fe_evt("rewind_suspend history_frames=%u",
          (unsigned)(fe_rewind_frame_now() - fe_rewind_frame_oldest()));
   /* Not rw_shutdown: no run totals, the test files stay open, and the
    * options stay for the resume.
    *
    * If a rewind was on screen the machine is not at the frame shown but
    * within two capture windows of it, re-running.  Bringing it there
    * would mean emulating frames from inside a power callback; instead
    * play goes on from where the machine IS.  The player loses the last
    * fraction of a second of a rewind he was still holding when the lid
    * closed. */
   if (rw_gesture_live())
   {
      /* NOT landed (fe_host_rewind_land was not called, or ran out of
       * budget).  The machine's own frame decides: */
      int bad = rw_distrust || fe_rewind_mach_noland();
      fe_evt("rewind_suspend_unlanded mach=%u now=%u top=%u vetted=%d",
             (unsigned)fe_rewind_frame_mach(), (unsigned)fe_rewind_frame_now(),
             (unsigned)fe_rewind_frame_top(), !bad);
      if (bad)
         rw_reset_owed = 1;    /* inside an old save: it may not go on */
      else
         rw_unlanded_ok++;
   }
   rw_distrust = 0;
   rw_resume_frame = fe_rewind_frame_mach();
   gpsp_rewind_clock_mode(0);
   if (rw_redo_high > rw_resume_frame)
      rw_redo_high = rw_resume_frame;   /* test: the input log is gone too */
   rw_on = 0;
   rw_mask_on = 0;
   rw_tee = 0;
   rw_gesture = 0;
   rw_present_valid = 0;
   rw_mem_drop(1);
   rw_suspended = 1;
}

int fe_host_rewind_resume(void *base, size_t bytes)
{
   fe_host_rewind_opts o;
   if (!rw_suspended || rw_on)
      return -1;
   o = rw_opt;
   rw_reset_if_owed("resume");
   fe_host_rewind_set_pool(base, bytes);
   /* enable opens the test log again: keep the one that is open */
   o.hash_path = NULL;
   if (fe_host_rewind_enable(&o) != 0)
   {
      rw_suspended = 0;
      return -1;
   }
   fe_evt("rewind_resume ok");
   return 0;
}

unsigned fe_host_rewind_resets(void)
{
   return rw_unvetted_resets;
}

void fe_host_rewind_distrust(void)
{
   rw_distrust = 1;
}

void fe_host_rewind_abandon(void)
{
   rw_reset_if_owed("abandon");
   rw_suspended = 0;
}

/* Emulation thread.  The game may not go on from where the machine was
 * left: start it again from the cartridge.  The save in memory is the
 * game's last whole save (no re-run frame wrote it) and reset_gba leaves it
 * alone, so this is a console switched off and on, not a lost save. */
static void rw_reset_if_owed(const char *where)
{
   (void)where;
   if (!rw_reset_owed)
      return;
   rw_reset_owed = 0;
   rw_unvetted_resets++;
   fe_evt("rewind_unvetted_reset at=%s frame=%u (the history ended with the "
          "machine inside an old save sequence)", where, (unsigned)frame_count);
   gba_rewind_backup_ro = 0;
   retro_reset();
   rw_bk_quiet = 0xFFFF;        /* another machine: its chip is idle */
   rw_wr_quiet = 0xFFFF;
   if (rw_on)
      rw_barrier(FE_RW_LIMIT_SLEEP);
}

/* ---- measurement lines ----------------------------------------------------
 * rewind_win, on the heartbeat cadence (with EVT fps and EVT core_prof): what
 * recording cost in the window.  core_prof times retro_run; work_us is all
 * the rewind does to a normal frame outside it (gather and pack included),
 * so the overhead per frame is work_us / frames.
 * rewind_gesture, when play resumes after a rewind: what the rewind cost. */
static uint64_t rw_work_us;
static uint32_t rw_work_max_us;
static struct
{
   unsigned frame;
   uint32_t snaps, evicted;
   uint64_t bytes, gather_us, pack_us, work_us, words_read, words_total;
} rw_w0;
static struct
{
   uint32_t show, stall, limit, catchup, restores, steps, replayed;
   uint64_t restore_us, unpack_us, replay_us, tick_us;
   unsigned tick_n;
   uint32_t from;
} rw_g0;
static uint32_t rw_g_tick_max, rw_g_restore_max;
static uint32_t rw_verify_seen;

static void rw_win_mark(void)
{
   const fe_rewind_stats *t = fe_rewind_get_stats();
   rw_w0.frame     = frame_count;
   rw_w0.snaps     = t->snapshots;
   rw_w0.evicted   = t->evicted;
   rw_w0.bytes     = t->packed_bytes;
   rw_w0.gather_us = t->gather_us;
   rw_w0.pack_us   = t->pack_us;
   rw_w0.work_us   = rw_work_us;
   rw_w0.words_read  = t->words_read;
   rw_w0.words_total = t->words_total;
   rw_work_max_us  = 0;
   fe_rewind_window_reset();
}

static void rw_win_evt(void)
{
   const fe_rewind_stats *t;
   unsigned n;
   if (!rw_on)
      return;
   t = fe_rewind_get_stats();
   n = t->snapshots - rw_w0.snaps;
   (void)n;
   fe_evt("rewind_win frames=%u snaps=%u gather_us=%u/%u pack_us=%u/%u "
          "work_us=%u/%u bytes=%u read_pct=%u evicted=%u ring_used=%u "
          "records=%u history_frames=%u save_frames=%u closed_frames=%u "
          "serial_frames=%u deferred=%u",
          frame_count - rw_w0.frame, n,
          n ? (unsigned)((t->gather_us - rw_w0.gather_us) / n) : 0,
          t->gather_win_max_us,
          n ? (unsigned)((t->pack_us - rw_w0.pack_us) / n) : 0,
          t->pack_win_max_us,
          (unsigned)(rw_work_us - rw_w0.work_us), rw_work_max_us,
          (unsigned)(t->packed_bytes - rw_w0.bytes),
          t->words_total != rw_w0.words_total
             ? (unsigned)((t->words_read - rw_w0.words_read) * 100u /
                          (t->words_total - rw_w0.words_total)) : 0u,
          t->evicted - rw_w0.evicted,
          (unsigned)fe_rewind_ring_used(), (unsigned)fe_rewind_ring_records(),
          (unsigned)(fe_rewind_frame_now() - fe_rewind_frame_oldest()),
          rw_save_write_frames - rw_w_save_write_frames,
          rw_bk_busy_frames - rw_w_bk_busy_frames,
          rw_serial_frames - rw_w_serial_frames,
          t->deferred - rw_w_deferred);
   /* How much can be rewound right now, and why not more: the frames held
    * as time; what the ring was given against what was asked; the distance
    * between snapshots in force; and the last thing that ended the
    * history (why@frame; "history" = nothing has since boot). */
   fe_evt("rewind_avail avail_ms=%u history_frames=%u ring_asked=%u "
          "ring_granted=%u ring_used=%u gap_frames=%u bytes_per_snap=%u "
          "last_drop=%s@%u drops: save=%u serial=%u cheat=%u",
          (unsigned)((uint64_t)(fe_rewind_frame_now() -
                                fe_rewind_frame_oldest()) * 1000000u / 59727u),
          (unsigned)(fe_rewind_frame_now() - fe_rewind_frame_oldest()),
          (unsigned)rw_ring_asked, (unsigned)rw_ring_granted,
          (unsigned)fe_rewind_ring_used(), (unsigned)fe_rewind_gap(),
          n ? (unsigned)((t->packed_bytes - rw_w0.bytes) / n) : 0,
          rw_why_name[rw_floor_why < FE_RW_LIMIT_N ? rw_floor_why : 4],
          (unsigned)rw_floor_frame, rw_rlog_trims + rw_rlog_resets, rw_barriers_serial,
          rw_barriers_cheat);
   rw_w_deferred = t->deferred;
#ifdef REWIND_COUNT
   {
      extern uint32_t gba_rewind_cnt[2];
      static uint32_t c0[2];
      unsigned fr = frame_count - rw_w0.frame;
      fe_evt("rewind_marks frames=%u ewram_stores=%u vram_stores=%u "
             "per_frame=%u/%u", fr, gba_rewind_cnt[0] - c0[0],
             gba_rewind_cnt[1] - c0[1],
             fr ? (gba_rewind_cnt[0] - c0[0]) / fr : 0,
             fr ? (gba_rewind_cnt[1] - c0[1]) / fr : 0);
      c0[0] = gba_rewind_cnt[0];
      c0[1] = gba_rewind_cnt[1];
   }
#endif
   rw_w_serial_frames = rw_serial_frames;
   rw_w_save_write_frames = rw_save_write_frames;
   rw_w_bk_busy_frames = rw_bk_busy_frames;
   if (rw_opt.verify)
   {
      if (t->verify_bad_words != rw_verify_seen)
         fe_evt("rewind_verify_fail snaps=%u words=%u first_off=%u "
                "(a written page was called clean)",
                t->verify_bad_snaps, t->verify_bad_words, t->verify_first_off);
      rw_verify_seen = t->verify_bad_words;
   }
   if (rw_opt.gather_prof && !rw_pieces)
   {
      /* Where the serialised gather's time goes (libretro.c): clearing the
       * image, the save-state writers, the sound in flight. */
      uint32_t g[4];
      gpsp_rewind_gprof(g, NULL);
      if (g[0])
         fe_evt("rewind_gprof n=%u clear_us=%u save_us=%u sound_us=%u",
                g[0], g[1] / g[0], g[2] / g[0], g[3] / g[0]);
   }
   rw_win_mark();
}

/* A rewind's ticks never block, so the save writer (below main) would not
 * run for as long as the control is held: on a PSP-3000 a save made just
 * before a hold stayed half written on the stick for 20 to 27 s.  The save
 * in memory cannot change during a rewind (it is write-protected), so there
 * is nothing to wait for: at the first tick of a press main sleeps until
 * the pending save is on the stick, for at most a quarter of a second (the
 * ring is not shown yet; a tap pays this only right after a save), and
 * every further tick gives the writer one sleep while anything is still
 * pending.  1 = it slept. */
static unsigned rw_flush_engage_waits, rw_flush_tick_yields;
static int rw_denied_now;
static uint32_t rw_flush_engage_max_us;
static int rw_flush_help(int engage)
{
   uint64_t t0;
   if (!host_io || gb_family || !&backup_write_gen || !host.save_path)
      return 0;
   if (sram_disk_gen == backup_write_gen)
      return 0;
   if (!sram_scan_gen_valid || sram_scan_gen != backup_write_gen || !sram_have_blk)
      if (!sram_scan_req)
         sram_scan_req = 1;
   t0 = host.time_us ? host.time_us() : 0;
   if (!engage)
   {
      rw_flush_tick_yields++;
      host_io->wake();
      host_io->yield();
      return 1;
   }
   rw_flush_engage_waits++;
   {
      unsigned n = 0;
      while (sram_disk_gen != backup_write_gen && n++ < 2000 &&
             (!t0 || host.time_us() - t0 < 250000u))
      {
         host_io->wake();
         host_io->yield();
      }
   }
   if (t0)
   {
      uint32_t d = (uint32_t)(host.time_us() - t0);
      if (d > rw_flush_engage_max_us)
         rw_flush_engage_max_us = d;
      fe_evt("rewind_flush_wait ms=%u on_disk=%d", (unsigned)(d / 1000u),
             sram_disk_gen == backup_write_gen);
   }
   return 1;
}

static void rw_gesture_mark(void)
{
   const fe_rewind_stats *t = fe_rewind_get_stats();
   rw_g0.show       = t->ticks_show;
   rw_g0.stall      = t->ticks_stall;
   rw_g0.limit      = t->ticks_limit;
   rw_g0.catchup    = t->ticks_catchup;
   rw_g0.restores   = t->restores;
   rw_g0.steps      = t->steps_back;
   rw_g0.replayed   = t->replayed;
   rw_g0.restore_us = t->restore_us;
   rw_g0.unpack_us  = t->unpack_us;
   rw_g0.replay_us  = t->replay_us;
   rw_g0.tick_us    = rw_tick_us;
   rw_g0.tick_n     = rw_tick_n;
   rw_g0.from       = fe_rewind_frame_now();
   rw_g_tick_max    = 0;
   rw_g_restore_max = 0;
}

extern void gpsp_rewind_rprof(uint32_t out[9]);

static void rw_gesture_evt(void)
{
   const fe_rewind_stats *t = fe_rewind_get_stats();
   unsigned ticks = rw_tick_n - rw_g0.tick_n;
   unsigned res = t->restores - rw_g0.restores;
   unsigned rep = t->replayed - rw_g0.replayed;
   unsigned stp = t->steps_back - rw_g0.steps;
   (void)ticks; (void)res; (void)rep; (void)stp;
   fe_evt("rewind_gesture from=%u to=%u ticks=%u show=%u stall=%u limit=%u "
          "catchup=%u tick_us=%u/%u restores=%u restore_us=%u unpack_us=%u "
          "replayed=%u replay_us=%u frames_per_tick_max=%u "
          "restore_kb=%u restore_arrays_us=%u code_kept=%u/%u nopic=%u "
          "resnaps=%u",
          (unsigned)rw_g0.from, (unsigned)fe_rewind_frame_now(), ticks,
          t->ticks_show - rw_g0.show, t->ticks_stall - rw_g0.stall,
          t->ticks_limit - rw_g0.limit, t->ticks_catchup - rw_g0.catchup,
          ticks ? (unsigned)((rw_tick_us - rw_g0.tick_us) / ticks) : 0,
          rw_g_tick_max,
          res, res ? (unsigned)((t->restore_us - rw_g0.restore_us) / res) : 0,
          stp ? (unsigned)((t->unpack_us - rw_g0.unpack_us) / stp) : 0,
          rep, rep ? (unsigned)((t->replay_us - rw_g0.replay_us) / rep) : 0,
          t->tick_frames_max,
          rw_restore_n ? (unsigned)(rw_restore_copied / rw_restore_n / 1024) : 0,
          rw_restore_n ? (unsigned)(rw_restore_arr_us / rw_restore_n) : 0,
          rw_restore_kept, rw_restore_n, rw_nopic_shown, t->resnaps);
   fe_evt("rewind_cost frame_us=%u rerun_pic_us=%u rerun_head_us=%u "
          "sound_mode=%d synth_skipped=%u head_skipped=%u", rw_e_frame,
          rw_e_pic, rw_e_head, rw_opt.sound_mode, rw_synth_skipped,
          rw_head_skipped);
   if (rw_opt.gather_prof)
   {
      /* Where a restore's time goes, part by part (means over the restores
       * since the last line; restore_us above is their sum plus the
       * timers).  load = the state loader on everything but the arrays,
       * split below it into cpu / input+main / memory / sound / tail. */
      uint32_t g[9];
      static uint32_t n0;
      static uint64_t arr0;
      uint32_t rn = rw_restore_n - n0;
      (void)rn; (void)arr0;
      gpsp_rewind_rprof(g);
      if (g[0])
         fe_evt("rewind_rprof n=%u jit_close_us=%u arrays_us=%u verify_us=%u "
                "rest_copy_us=%u sound_clear_us=%u load_us=%u "
                "(cpu=%u inmain=%u memory=%u sound=%u tail=%u) maps_us=%u "
                "audio_us=%u", g[0],
                (unsigned)(rw_rs_jit_us / g[0]),
                rn ? (unsigned)((rw_restore_arr_us - arr0) / rn) : 0,
                (unsigned)(rw_rs_verify_us / g[0]),
                g[3] / g[0], g[1] / g[0], g[2] / g[0],
                g[4] / g[0], g[5] / g[0], g[6] / g[0], g[7] / g[0],
                g[8] / g[0],
                (unsigned)(rw_rs_maps_us / g[0]),
                (unsigned)(rw_rs_aud_us / g[0]));
      n0 = rw_restore_n;
      arr0 = rw_restore_arr_us;
      rw_rs_jit_us = rw_rs_verify_us = rw_rs_maps_us = rw_rs_aud_us = 0;
   }
}

static uint32_t rw_stepped;

int fe_host_rewind_tick(int held)
{
   return fe_host_rewind_tick_step(held, 1);
}

void fe_host_rewind_set_tick_budget(uint32_t us)
{
   if (rw_on)
      fe_rewind_set_budget(us);
}

void fe_host_rewind_set_head_skip(int on)
{
   rw_head_skip = on;
}

void fe_host_rewind_set_cadence(uint32_t stride, int quiet)
{
   if (!rw_on)
      return;
   fe_rewind_set_stride(stride);
   rw_cad_quiet = quiet;
}

int fe_host_rewind_tick_step(int held, uint32_t step)
{
   fe_rewind_shown s;
   uint64_t t0;
   int r;

   if (fe_host_rewind_blocked())
   {
      /* Refused from here on (a link session began).  If that happened in
       * the middle of a rewind the machine is on some old frame: the
       * control is let go and the rewind LANDS first, as after any release
       * (returning FE_RW_PLAY here let the game go on from that frame, with
       * the save writable again). */
      if (!rw_on || !rw_gesture_live())
         return FE_RW_PLAY;
      held = 0;
   }
   if (held && !rw_gesture && rw_wr_quiet < rw_opt.save_quiet &&
       !(rw_opt.sav_mutant & 4))
   {
      /* the game is saving: not now (see rw_wr_quiet) */
      if (!rw_denied_now)
      {
         rw_denied_now = 1;
         rw_denied_saving++;
         fe_evt("rewind_denied why=saving frames_since_write=%u",
                (unsigned)rw_wr_quiet);
      }
      held = 0;
   }
   else if (!held)
      rw_denied_now = 0;
   if (held && !rw_gesture)
   {
      (void)rw_flush_help(1);   /* the game's own save reaches the stick first */
      rw_gesture_mark();        /* before the tick: its work counts too */
   }
   else if (held || rw_gesture)
      (void)rw_flush_help(0);
   t0 = rw_now();
   s.pix = NULL;
   rw_held_tick = held;
   r = fe_rewind_tick_step(held, step, &s, &rw_stepped);
   rw_held_tick = 0;
   if (gba_rewind_rlog_miss != rw_rlog_miss_seen && r != FE_RW_ERROR)
   {
      /* A re-run frame read save bytes the first pass did not read: it is
       * not the frame it claims to be (or the log was cut under it).  It
       * got the live bytes.  Said and counted, as up to 4.0.0: resetting
       * the player's game in the middle of a rewind for a frame that may
       * differ is the worse failure (4.1.1). */
      rw_rlog_miss_seen = gba_rewind_rlog_miss;
      fe_evt("rewind_save_read_unlogged n=%u frame=%u",
             (unsigned)gba_rewind_rlog_miss, (unsigned)fe_rewind_frame_mach());
   }
   if (r == FE_RW_ERROR)
   {
      /* A failed restore leaves a machine that has not earned permission
       * to write the live save.  Reset from the cartridge before reporting
       * PLAY. retro_reset keeps the current battery bytes; replay never
       * wrote those bytes. */
      fe_evt("rewind_replay_fault restore=%d read_misses=%u frame=%u",
             r == FE_RW_ERROR, (unsigned)gba_rewind_rlog_miss,
             (unsigned)fe_rewind_frame_mach());
      rw_rlog_miss_seen = gba_rewind_rlog_miss;
      rw_jit_close();
      rw_gesture = 0;
      rw_present_valid = 0;
      rw_mask_on = 0;
      rw_tee = 0;
      rw_distrust = 0;
      rw_reset_owed = 1;
      rw_reset_if_owed("replay_fault");
      return FE_RW_PLAY;
   }
   if (gba_rewind_backup_ro_hits != rw_ro_hits_seen)
   {
      /* Re-run frames passed over save writes: refused, as they must be
       * (the save is the game's last word, not the rewind's). */
      rw_replay_save_hits += gba_rewind_backup_ro_hits - rw_ro_hits_seen;
      rw_ro_hits_seen = gba_rewind_backup_ro_hits;
   }
   if (r == FE_RW_PLAY)
   {
      /* (a snapshot that was due is taken in there too: part of what
       * recording costs a normal frame) */
      if (host.time_us)
      {
         uint32_t d = (uint32_t)(rw_now() - t0);
         rw_work_us += d;
         if (d > rw_work_max_us)
            rw_work_max_us = d;
      }
      rw_limit_said = 0;
      if (rw_gesture)
      {
         /* Play resumes, from the frame on screen. */
         rw_gesture = 0;
         rw_jit_close();
         rw_gesture_evt();
         /* From here on the save is read as it is, and logged from where
          * the frames just re-run left the log. */
         gba_rewind_rlog_end = gba_rewind_rlog_pos;
         rw_rlog_pos_seen = gba_rewind_rlog_pos;
         /* (the log's end went back with the timeline: so does the chunk it
          * is in; the frames noted for the chunks up to it still hold) */
         rw_rlog_chunk = gba_rewind_rlog_pos
            ? (gba_rewind_rlog_pos - 1) / (rw_rlog_bytes / RW_RLOG_CHUNKS) : 0;
         gba_rewind_bk_acc = 0;
         if (rw_hash_fp)
            fprintf(rw_hash_fp, "E %u %u %08x\n",
                    (unsigned)fe_rewind_frame_now(),
                    (unsigned)gba_rewind_backup_busy(), (unsigned)rw_sav_crc());
      }
      return r;
   }
   if (fe_rewind_at_limit())
   {
      /* Held with nothing earlier to show: say once per press where the
       * history starts and why (the readout's stop state). */
      if (!rw_limit_said)
      {
         const char *const *why = rw_why_name;
         (void)why;
         fe_host_rewind_info inf;
         fe_host_rewind_info_get(&inf);
         fe_evt("rewind_limit frame=%u why=%s", (unsigned)fe_rewind_frame_now(),
                why[inf.limit_reason]);
         rw_limit_said = 1;
      }
   }
   else if (!held)
      rw_limit_said = 0;
   rw_gesture = 1;
   if (rw_opt.no_capture)
   {
      /* Nothing was kept: the picture holds.  The timeline still moved. */
      if (r == FE_RW_SHOW && rw_hash_fp)
         fprintf(rw_hash_fp, "S %u\n", (unsigned)s.frame);
   }
   else if (r == FE_RW_SHOW)
   {
      if (s.pix)
      {
         unsigned y;
         uint32_t vc = 0;
         if (!rw_opt.show || rw_hash_fp)
            for (y = 0; y < FE_GBA_HEIGHT; y++)
            {
               memcpy(rw_present + y * FE_GBA_WIDTH, s.pix + y * rw_pitch,
                      FE_GBA_WIDTH * 2);
               vc = fe_crc32(vc, s.pix + y * rw_pitch, FE_GBA_WIDTH * 2);
            }
         rw_present_valid = 1;
         last_frame = rw_present;
         last_pitch = FE_GBA_WIDTH * 2;
         if (rw_hash_fp && (rw_opt.sound_mode || s.pix_frame != s.frame))
            /* (a rewind that keeps no sound, or a picture every few
             * frames: the picture only, and which frame it is of) */
            fprintf(rw_hash_fp, "R %u %08x silent 0 %u\n", (unsigned)s.frame,
                    (unsigned)vc, (unsigned)s.pix_frame);
         else if (rw_hash_fp)
            fprintf(rw_hash_fp, "R %u %08x %08x %u\n", (unsigned)s.frame,
                    (unsigned)vc, (unsigned)fe_crc32(0, s.aud, s.aud_n * 4),
                    (unsigned)s.aud_n);
      }
      else
      {
         rw_nopic_shown++;
         if (rw_hash_fp)        /* a frame that kept no picture: a step */
            fprintf(rw_hash_fp, "S %u\n", (unsigned)s.frame);
      }
      if (rw_opt.show)
         rw_opt.show(s.pix);
      if (host.audio_frames && s.aud_n && !rw_opt.sound_mode && !rw_cad_quiet)
         host.audio_frames(s.aud, s.aud_n);
   }
   /* Every rewind tick presents, so the platform's readout stays live while
    * the picture holds. */
   if (host.video_frame && !rw_opt.no_capture && !rw_opt.show)
      host.video_frame(rw_present_valid ? rw_present : NULL, FE_GBA_WIDTH,
                       FE_GBA_HEIGHT, FE_GBA_WIDTH * 2);
   if (host.time_us)
   {
      uint32_t d = (uint32_t)(rw_now() - t0);
      rw_tick_us += d;
      rw_tick_n++;
      if (d > rw_tick_max_us)
         rw_tick_max_us = d;
      if (d > rw_g_tick_max)
         rw_g_tick_max = d;
   }
   return r;
}

/* A rewind is in progress: a tick has answered something other than PLAY
 * and none has answered PLAY since.  (Not fe_rewind_active(): that stays
 * true after the landing until the first frame is played.) */
static int rw_gesture_live(void)
{
   return rw_gesture;
}

int fe_host_rewind_busy(void)
{
   return rw_on && rw_gesture_live();
}

int fe_host_rewind_unvetted(void)
{
   return rw_on && rw_gesture_live() && fe_rewind_mach_noland();
}

int fe_host_rewind_land(uint32_t budget_us)
{
   uint64_t t0 = rw_now();
   uint32_t n = 0, from;
   int ok = 1;

   if (!rw_on || !rw_gesture_live())
      return 1;
   from = fe_rewind_frame_mach();
   /* The control is up: every tick snaps `now` to a frame play may continue
    * from and brings the machine nearer to it; FE_RW_PLAY = it is there. */
   while (fe_host_rewind_tick_step(0, 1) != FE_RW_PLAY)
   {
      n++;
      if ((budget_us && host.time_us &&
           (uint32_t)(rw_now() - t0) >= budget_us) || n >= 100000u)
      {
         ok = 0;
         break;
      }
   }
   if (ok)
      rw_lands++;
   else
      rw_land_fails++;
   (void)from;
   fe_evt("rewind_land ok=%d ticks=%u us=%u mach=%u->%u now=%u budget_us=%u",
          ok, (unsigned)n, (unsigned)(rw_now() - t0), (unsigned)from,
          (unsigned)fe_rewind_frame_mach(), (unsigned)fe_rewind_frame_now(),
          (unsigned)budget_us);
   return ok;
}

void fe_host_rewind_info_get(fe_host_rewind_info *out)
{
   memset(out, 0, sizeof(*out));
   if (!rw_on)
      return;
   out->active       = fe_rewind_active();
   out->back_frames  = fe_rewind_frame_top() - fe_rewind_frame_now();
   out->avail_frames = fe_rewind_frame_now() - fe_rewind_frame_oldest();
   out->ring_used    = fe_rewind_ring_used();
   out->ring_bytes   = rw_opt.ring_bytes;
   out->records      = fe_rewind_ring_records();
   out->at_limit     = fe_rewind_at_limit();
   out->stepped      = rw_stepped;
   out->now_frame    = fe_rewind_frame_now();
   out->mach_frame   = fe_rewind_frame_mach();
   out->ring_granted  = rw_ring_granted;
   out->frame_us      = rw_e_frame;
   out->rerun_pic_us  = rw_e_pic;
   out->rerun_head_us = rw_e_head;
   {
      const fe_rewind_stats *t = fe_rewind_get_stats();
      out->restore_us = t->restores
         ? (uint32_t)((t->restore_us + t->unpack_us) / t->restores) : 0;
   }
   /* The stop is the state load (or the cheat change, ...) only if the
    * oldest frame still held is the one the history started at; once the
    * ring has dropped older snapshots the stop is simply how much it holds. */
   out->limit_reason = fe_rewind_frame_oldest() <= rw_floor_frame + 2
                          ? rw_floor_why : FE_RW_LIMIT_HISTORY;
}

static void rw_shutdown(void)
{
   const fe_rewind_stats *t;
   if (!rw_on)
   {
      /* reserved and never enabled, or suspended: nothing to report, but
       * the memory and the pool still have to be let go */
      rw_mem_drop(0);
      rw_suspended = 0;
      rw_reset_owed = 0;        /* the game is going away: nothing to reset */
      gpsp_rewind_marks_want(0);
      if (rw_hash_fp)
         fclose(rw_hash_fp);
      rw_hash_fp = NULL;
      free(rw_hash_state);
      rw_hash_state = NULL;
      return;
   }
   t = fe_rewind_get_stats();
   (void)t;
   /* the history ended in the middle of a rewind: landed first / could not /
    * went on from the machine's own (vetted) frame / the game was reset */
   fe_evt("rewind_ends lands=%u land_fails=%u unlanded_ok=%u "
          "unvetted_resets=%u", rw_lands, rw_land_fails, rw_unlanded_ok,
          rw_unvetted_resets);
   rw_reset_owed = 0;
   fe_evt("rewind_stats snaps=%u bytes=%llu max=%u evicted=%u overflow=%u "
          "gather_us=%llu/%u pack_us=%llu/%u ring_used=%u records=%u "
          "history_frames=%u",
          t->snapshots, (unsigned long long)t->packed_bytes, t->packed_max,
          t->evicted, t->overflow_resets,
          (unsigned long long)t->gather_us, t->gather_max_us,
          (unsigned long long)t->pack_us, t->pack_max_us,
          (unsigned)fe_rewind_ring_used(), (unsigned)fe_rewind_ring_records(),
          (unsigned)(fe_rewind_frame_now() - fe_rewind_frame_oldest()));
   fe_evt("rewind_use engages=%u show=%u stall=%u limit=%u catchup=%u "
          "catchup_run_max=%u steps_back=%u unpack_us=%llu/%u restores=%u "
          "restore_us=%llu/%u replayed=%u replay_us=%llu audio_clipped=%u "
          "save_barriers=%u cheat_barriers=%u serial_barriers=%u "
          "replay_save_hits=%u "
          "jit_rom_emit=%llu jit_ram_emit=%llu "
          "snaps_back=%u snaps_fwd=%u snap_max=%u",
          t->engages, t->ticks_show, t->ticks_stall, t->ticks_limit,
          t->ticks_catchup, t->catchup_run_max, t->steps_back,
          (unsigned long long)t->unpack_us, t->unpack_max_us, t->restores,
          (unsigned long long)t->restore_us, t->restore_max_us, t->replayed,
          (unsigned long long)t->replay_us, t->audio_clipped, rw_rlog_trims + rw_rlog_resets,
          rw_barriers_cheat, rw_barriers_serial, rw_replay_save_hits,
          (unsigned long long)rw_jit_rom_emit,
          (unsigned long long)rw_jit_ram_emit,
          t->snaps_back, t->snaps_fwd, t->snap_max);
   /* The save under rewind: frames that wrote it / touched the chip; bytes
    * of it the game read (first pass) and re-runs were served from the log;
    * reads with nothing logged (must be 0); history cut because the log is
    * a ring (trims keep the newer part, resets do not). */
   fe_evt("rewind_save write_frames=%u busy_frames=%u read_bytes=%u "
          "rerun_served=%u unlogged=%u rerun_writes_refused=%u "
          "rlog_bytes=%u rlog_trims=%u rlog_resets=%u busy_now=%u "
          "gaps=%u/%u/%u/%u/%u/%u/%u/%u flush_waits=%u/%ums flush_yields=%u "
          "denied_saving=%u",
          rw_save_write_frames, rw_bk_busy_frames,
          (unsigned)gba_rewind_rlog_end, (unsigned)gba_rewind_rlog_served,
          (unsigned)gba_rewind_rlog_miss, rw_replay_save_hits,
          (unsigned)rw_rlog_bytes, rw_rlog_trims, rw_rlog_resets,
          (unsigned)gba_rewind_backup_busy(),
          rw_bk_gaps[0], rw_bk_gaps[1], rw_bk_gaps[2], rw_bk_gaps[3],
          rw_bk_gaps[4], rw_bk_gaps[5], rw_bk_gaps[6], rw_bk_gaps[7],
          rw_flush_engage_waits, (unsigned)(rw_flush_engage_max_us / 1000u),
          rw_flush_tick_yields, rw_denied_saving);
   /* The bound that makes tapping harmless, and what it cost here.  core =
    * inside fe_rewind_tick; host adds presenting the frame. */
   fe_evt("rewind_tick n=%u mean_us=%u host_max_us=%u core_max_us=%u "
          "frames_max=%u restores_max=%u engage_us=%llu/%u anomalies=%u "
          "selfcheck=%d verify=%u/%u restore_verify_bad=%u",
          rw_tick_n, rw_tick_n ? (unsigned)(rw_tick_us / rw_tick_n) : 0,
          rw_tick_max_us, t->tick_max_us, t->tick_frames_max,
          t->tick_restores_max, (unsigned long long)t->engage_us,
          t->engage_max_us, t->anomalies + rw_restore_bad,
          fe_rewind_selfcheck(), t->verify_bad_words, t->verify_runs,
          rw_restore_bad);
   if (rw_hash_fp)
      fclose(rw_hash_fp);
   rw_hash_fp = NULL;
   free(rw_hash_state);
   rw_hash_state = NULL;
   rw_mem_drop(0);
   gpsp_rewind_clock_mode(0);
   gpsp_rewind_marks_want(0);
   rw_on = 0;
   rw_mask_on = 0;
}
#endif /* REWIND */

#ifdef GPSP_CHEATS
#include "fe_cheats.h"
/* One Game Boy cheat write (fe_cheats_gb_frame).  gbcore_poke refuses ROM
 * and I/O itself; fe_cheats has already refused everything but work RAM and
 * high RAM, so cartridge (save) RAM is never asked for. */
static int gb_cheat_poke(void *ctx, uint16_t addr, uint8_t value)
{
   return gbcore_poke((gbcore_t *)ctx, addr, &value, 1);
}
#endif

void fe_host_run_frame(void)
{
   uint64_t core_t0 = host.time_us ? host.time_us() : 0;
   unsigned rf0 = 0, ff0 = 0, sf0 = 0, df0 = 0, pl0 = 0;

#ifdef GBA_LINK
   if (gbl && gba_cable)
   {
      gba_cable_frame(core_t0);
      return;
   }
   if (gba_cable && gbl_hold)
      return;                  /* harness: the loaded state waits for the link */
#endif
   if (gbl)
   {
      gblink_frame(core_t0);
      return;
   }
   if (gb_core && gbl_hold)
      return;                  /* harness: waiting for the link to form */
   if (gb_core)
   {
      static unsigned skip_phase;
      unsigned interval = gb_frameskip_interval();
      uint32_t pad = (host.input_bitmask ? host.input_bitmask() : 0) |
                     injected_mask;
      if (interval)
         gb_skip_frame = (skip_phase++ % (interval + 1)) != 0;
      else
      {
         gb_skip_frame = 0;
         skip_phase = 0;
      }
      if (skip_override >= 0)
         gb_skip_frame = skip_override;
      gbcore_set_skip_render(gb_core, gb_skip_frame);
      if (gb_dual)
      {
         gb_dual_buttons = gb_buttons_from_retro(pad);
         if (gbdual_advance(gb_dual, gb_dual_input, NULL) != 1)
            fe_evt("gb_dual_advance FAILED frame=%u", frame_count);
      }
      else
      {
#ifdef GPSP_CHEATS
         /* Game Boy cheats (docs/CHEATS.md): the enabled GameShark writes,
          * once per emulated frame, before the core runs it.  Never in the
          * dual (link) path above; a no-op with nothing switched on. */
         fe_cheats_gb_frame(gb_cheat_poke, gb_core);
#endif
         if (gbcore_run_frame(gb_core, gb_buttons_from_retro(pad)) != 0)
            fe_evt("gb_run_frame FAILED frame=%u", frame_count);
      }
      if (core_t0 && host.time_us)
      {
         uint32_t elapsed = (uint32_t)(host.time_us() - core_t0);
         core_run_us_total += elapsed;
         core_run_calls++;
         if (elapsed > core_run_max_us) core_run_max_us = elapsed;
         if (elapsed > core_win_max_us) core_win_max_us = elapsed;
      }
      frame_count++;
      fe_host_sram_service();
      if ((frame_count % hb_every) == 0)
      {
         fe_evt("heartbeat frames=%u t_us=%llu", frame_count,
                (unsigned long long)(host.time_us ? host.time_us() : 0));
         fps_evt();
         core_prof_evt();
         if (gb_dual)
            fe_evt("gb_dual frames=%llu/%llu lines=%llu serial=%llu/%llu",
                   (unsigned long long)gbdual_frame(gb_dual, 0),
                   (unsigned long long)gbdual_frame(gb_dual, 1),
                   (unsigned long long)gbdual_lines(gb_dual),
                   (unsigned long long)gbdual_serial_bytes(gb_dual, 0),
                   (unsigned long long)gbdual_serial_bytes(gb_dual, 1));
      }
      return;
   }

   if (core_t0 && host.core_counters)
      host.core_counters(&rf0, &ff0, &sf0, &df0, &pl0);

#ifdef GBA_LINK
   if (host.gba_dual && !host.gba_dual_session && !fe_gbadual_active())
      gba_dual_bench_idle();
   if (fe_gbadual_active())
      fe_gbadual_frame(fe_gbadual_keys_from_retro(
         (host.input_bitmask ? host.input_bitmask() : 0) | injected_mask));
   else
#endif
   {
#ifdef REWIND
   rw_latch_input();
#endif
   retro_run();
   }
   fe_host_guest_fault_check();

   if (core_t0)
   {
      uint32_t d = (uint32_t)(host.time_us() - core_t0);
#ifdef REWIND
      rw_frame_core_us = d;
      if (!rw_ff)
         rw_ema(&rw_e_frame, d);
#endif
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
#ifdef REWIND
   {
      /* Everything rewind adds to a normal frame OUTSIDE retro_run (which
       * core_prof times): the input log, a snapshot when one is due. */
      uint64_t w0 = rw_on && host.time_us ? host.time_us() : 0;
      rw_after_frame();
      if (w0)
      {
         uint32_t d = (uint32_t)(host.time_us() - w0);
         rw_work_us += d;
         if (d > rw_work_max_us)
            rw_work_max_us = d;
      }
   }
#endif

   /* Keep an in-progress save write moving (ADR-0020) — a few 4 KiB blocks
    * per frame under a wall-clock budget, never a 128 KiB stall.  Both of
    * these are no-ops once a writer thread is installed: since ADR-0026 it
    * owns the scan cadence as well as the writes, so the emulation thread
    * pays exactly nothing for the save path. */
   fe_host_sram_service();
   if (!host_io && !gb_family && (frame_count % SRAM_CHECK_INTERVAL) == 0)
      fe_host_sram_flush(0);   /* GB: fe_host_sram_service owns its cadence */
   if ((frame_count % hb_every) == 0)
   {
      fe_evt("heartbeat frames=%u t_us=%llu", frame_count,
             (unsigned long long)(host.time_us ? host.time_us() : 0));
      fps_evt();
      core_prof_evt();
      smc_addr_evt();
      smc_block_evt();
      core_phase_evt();
#ifdef REWIND
      rw_win_evt();
#endif
#ifdef GBA_LINK
      fe_gbadual_stats_evt(gbadual_evt);
#endif
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
   return !gb_family && netpacket_registered ? (const void *)&netpacket_cb : NULL;
}

/* The save half of fe_host_shutdown, on its own so the host tests can run
 * exactly what an exit runs (tools/tests/test_sram_exit_retry.c). */
static void sram_shutdown(void)
{
   fe_host_sram_flush(0);
   fe_host_sram_sync();     /* exit must never close over pending blocks */
   sram_close();
}

void fe_host_shutdown(void)
{
   /* ADR-0025 ordering contract: the writer thread is already stopped and
    * fe_host_set_io(NULL) already called, so everything below is
    * single-threaded — which is what makes it safe to close the file and
    * then let retro_unload_game() free the buffer the writer was reading. */
   sram_shutdown();
#ifdef REWIND
   rw_shutdown();
#endif
   if (gbl)
   {
      /* Quitting mid-session: nothing is committed (fe_gblink.h). */
      fe_evt("gblink_abandoned state=%d", fe_gblink_state(gbl));
      fe_gblink_destroy(gbl);
      gbl = NULL;
   }
   if (gb_family)
   {
      if (gb_dual)
         gbdual_destroy(gb_dual);
      else
         gbcore_shutdown(gb_core);
      gb_dual = NULL;
      gb_core = NULL;
      (void)gb_io_settle();
      free(gb_save_buffer);
      gb_save_buffer = NULL;
      gb_save_capacity = 0;
      if (!gb_io_req)
      {
         free(gb_io_buffer);
         gb_io_buffer = NULL;
         gb_io_capacity = 0;
      }
      gb_io_failed = 0;
      gb_dirty_since = 0;
      gb_save_crc_valid = 0;
      gb_save_file_len = -1;
      gb_link_reset();
      last_frame = NULL;
      last_pitch = 0;
      return;
   }
   retro_unload_game();
   retro_deinit();
}
