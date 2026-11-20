/* test_gb_rtc_utc.c -- the GB/GBC clock save's timestamp is UTC, and the
 * saves 3.5.0 and earlier stamped with local time still load without a jump
 * (frontend-common/fe_host.c "the clock trailer's stamp is
 * UTC").
 *
 * The production frontend (fe_host.c) boots a generated MBC3 clock
 * cartridge over a save file this test wrote, and the file it writes back is
 * read as another emulator would read it.  The test owns three things:
 *
 *   utc      the true time
 *   zone     how far the console's clock is ahead of UTC (its time zone)
 *   upgrade  the console's local time when it first ran a release that
 *            stamps UTC (CONFIG.INI gb_rtc_utc_since on the PSP)
 *
 * and plays the other parties itself: a UTC emulator (VBA-M, BGB, SameBoy,
 * mGBA) and GBAdhoc 3.5.0, which stamped the console's LOCAL time.  Every
 * case runs at eight zones from UTC-8 to UTC+13, two of them not whole
 * hours.  The clock is checked as elapsed seconds: an error of the zone
 * offset, which is what this is about, cannot hide.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../frontend-common/fe_host.h"
#include "../../frontend-common/netpacket_host.h"
#include "../../libretro/libretro-common/include/libretro.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s (zone %+lld s)\n", __FILE__, __LINE__, \
           #x, zone); return 1; \
} } while (0)

#define RAM 0x8000u
#define DAY 86400ll
#define UPGRADE_UTC 1790000000ll      /* the moment of the upgrade */

static long long utc, zone, upgrade;
static int legacy_reads = 1;          /* 0: the old clock has no reading */
static time_t clock_utc(void) { return (time_t)utc; }
static time_t clock_local(void)
{ return legacy_reads ? (time_t)(utc + zone) : (time_t)0; }

static char rom_path[128], save_path[160];
static uint8_t file[RAM + 64];

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
          ((uint32_t)p[3] << 24);
}

static int make_rom(uint8_t cart_type)
{
   static const uint8_t logo[48] = {
      0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
      0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
      0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
      0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
   };
   static uint8_t rom[0x8000];
   char path[] = "/tmp/gbadhocutcXXXXXX";
   unsigned i;
   uint8_t check = 0;
   int fd = mkstemp(path);
   if (fd < 0)
      return -1;
   memset(rom, 0, sizeof(rom));
   rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
   memcpy(rom + 0x104, logo, sizeof(logo));
   memcpy(rom + 0x134, "RTC UTC TEST", 12);
   rom[0x143] = 0x80;
   rom[0x147] = cart_type;
   rom[0x149] = 3;                    /* 32 KiB */
   for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - rom[i] - 1);
   rom[0x14D] = check;
   rom[0x150] = 0x18; rom[0x151] = 0xFE;          /* JR self */
   if (write(fd, rom, sizeof(rom)) != (ssize_t)sizeof(rom))
      return -1;
   close(fd);
   snprintf(rom_path, sizeof(rom_path), "%s.gbc", path);
   snprintf(save_path, sizeof(save_path), "%s.gbc.sav", path);
   return rename(path, rom_path);
}

/* ---- the save file, as each party writes and reads it ---- */

static long long file_count(void)
{
   const uint8_t *t = file + RAM;
   return (long long)get32(t) + get32(t + 4) * 60ll + get32(t + 8) * 3600ll +
          (long long)((get32(t + 12) & 0xff) | ((get32(t + 16) & 1) << 8)) *
             DAY;
}

static long long file_stamp(void)
{
   return (long long)get32(file + RAM + 40) |
          ((long long)get32(file + RAM + 44) << 32);
}

/* The live flag bits: 0x80 day carry, 0x40 halted. */
static unsigned file_flags(void)
{
   return get32(file + RAM + 16) & 0xC0;
}

static int file_marked(void)
{
   return memcmp(file + RAM + 37, "UTC", 3) == 0;
}

/* A file with the clock at `count`, stamped `stamp`.  `marked`: as this
 * frontend writes it. */
static int write_save(long long count, long long stamp, unsigned trailer,
                      int marked)
{
   uint8_t *t = file + RAM;
   FILE *f;
   unsigned i;
   int ok;
   for (i = 0; i < RAM; i++) file[i] = (uint8_t)(i * 5 + 2);
   memset(t, 0, 64);
   put32(t, (uint32_t)(count % 60));
   put32(t + 4, (uint32_t)(count / 60 % 60));
   put32(t + 8, (uint32_t)(count / 3600 % 24));
   put32(t + 12, (uint32_t)(count / DAY) & 0xff);
   put32(t + 16, (uint32_t)(count / DAY) >> 8 & 1);
   for (i = 0; i < 20; i += 4)
      put32(t + 20 + i, get32(t + i));
   put32(t + 40, (uint32_t)((uint64_t)stamp & 0xffffffffu));
   put32(t + 44, (uint32_t)((uint64_t)stamp >> 32));
   if (marked)
      memcpy(t + 37, "UTC", 3);
   f = fopen(save_path, "wb");
   ok = f && fwrite(file, 1, RAM + trailer, f) == RAM + trailer;
   if (f) fclose(f);
   return ok ? 0 : -1;
}

static long read_save(void)
{
   FILE *f = fopen(save_path, "rb");
   long n;
   if (!f)
      return -1;
   memset(file, 0xEE, sizeof(file));
   n = (long)fread(file, 1, sizeof(file), f);
   fclose(f);
   return n;
}

/* GBAdhoc 3.5.0 saving: the registers, and the console's LOCAL time. */
static int old_gbadhoc_saves(long long count)
{
   return write_save(count, utc + zone, 48, 0);
}

/* Another emulator saving: the registers and UTC, in 48 or 44 bytes. */
static int utc_emulator_saves(long long count, unsigned trailer)
{
   return write_save(count, utc, trailer, 0);
}

/* Another emulator loading the file on disk: the clock it shows.  It reads
 * each register from the low byte of its field and adds the time since the
 * stamp, taken as UTC; a stamp in its future adds nothing. */
static long long utc_emulator_loads(void)
{
   const uint8_t *t = file + RAM;
   long long c, stamp;
   if (read_save() != (long)(RAM + 48))
      return -1;
   c = (long long)t[0] + t[4] * 60ll + t[8] * 3600ll +
       (long long)(t[12] | ((t[16] & 1) << 8)) * DAY;
   stamp = file_stamp();
   return stamp < utc ? c + (utc - stamp) : c;
}

/* ---- this frontend ---- */

static void video(const uint16_t *p, unsigned w, unsigned h, size_t pitch)
{ (void)p; (void)w; (void)h; (void)pitch; }
static void audio(const int16_t *p, size_t n) { (void)p; (void)n; }
static uint64_t tick;
static uint64_t time_us(void) { tick += 16683; return tick; }
static uint32_t input(void) { return 0; }

static int with_legacy = 1;           /* the PSP's configuration */

static int boot(void)
{
   fe_host_config cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.console = FE_CONSOLE_GBC;
   cfg.rom_path = rom_path;
   cfg.save_path = save_path;
   cfg.video_frame = video;
   cfg.audio_frames = audio;
   cfg.input_bitmask = input;
   cfg.time_us = time_us;
   cfg.wallclock = clock_utc;
   if (with_legacy)
   {
      cfg.wallclock_legacy = clock_local;
      cfg.gb_rtc_legacy_before = upgrade;
   }
   if (fe_host_boot(&cfg) != 0)
      return -1;
   fe_host_run_frame();
   return 0;
}

/* The clock the game has right now, through the file a forced save writes
 * (the live registers are the clock at that instant). */
static long long play_and_save(void)
{
   long long c;
   if (boot() != 0 || fe_host_sram_flush(1) != 1)
      return -1;
   fe_host_shutdown();
   if (read_save() != (long)(RAM + 48))
      return -2;
   c = file_count();
   return c;
}

/* ---- the cases ---- */

#define C0 (37 * DAY + 14 * 3600 + 25 * 60 + 36)   /* day 37, 14:25:36 */

/* What this frontend writes: 48 bytes, UTC, marked, and nothing else out
 * of the format's "registers are bytes, the rest 0". */
static int test_export(void)
{
   unsigned i;
   utc = UPGRADE_UTC + 5 * DAY;
   CHECK(utc_emulator_saves(C0, 48) == 0);
   CHECK(play_and_save() == C0);
   CHECK(file_stamp() == utc);
   CHECK(file_marked());
   for (i = 0; i < 40; i += 4)
      CHECK((get32(file + RAM + i) & (i == 36 ? 0xffu : ~0u)) <= 0xff);
   for (i = 0; i < RAM; i++)
      CHECK(file[i] == (uint8_t)(i * 5 + 2));
   /* Carried to a UTC emulator 2 h 10 s later. */
   utc += 7210;
   CHECK(utc_emulator_loads() == C0 + 7210);
   return 0;
}

/* A save from a UTC emulator, written ten minutes ago.  West of UTC that
 * stamp is ahead of the console's local clock, which 3.5.0 took for "from
 * the future" and reset the clock. */
static int test_import(unsigned trailer)
{
   utc = UPGRADE_UTC + 9 * DAY;
   CHECK(utc_emulator_saves(C0, trailer) == 0);
   utc += 600;
   CHECK(play_and_save() == C0 + 600);
   CHECK(file_stamp() == utc && file_marked());
   /* And one written a second ago, and this very second. */
   CHECK(utc_emulator_saves(C0, trailer) == 0);
   utc += 1;
   CHECK(play_and_save() == C0 + 1);
   CHECK(utc_emulator_saves(C0, trailer) == 0);
   CHECK(play_and_save() == C0);
   return 0;
}

/* THE MIGRATION.  A save 3.5.0 wrote on this console (local-time stamp, no
 * mark), loaded for the first time by this release: the clock has moved by
 * the time the console was off and not by the zone offset. */
static int test_old_save(void)
{
   static const long long before[] = { 1, 300, 3600, 9 * 3600, 14 * 3600,
                                       40 * DAY };
   static const long long off[] = { 1, 600, 3 * 3600, 20 * 3600, 3 * DAY };
   unsigned a, b;
   long n;
   for (a = 0; a < sizeof(before) / sizeof(before[0]); a++)
      for (b = 0; b < sizeof(off) / sizeof(off[0]); b++)
      {
         /* Saved on 3.5.0 `before` seconds before the upgrade ... */
         utc = UPGRADE_UTC - before[a];
         CHECK(old_gbadhoc_saves(C0) == 0);
         /* ... and first loaded `off` seconds after it. */
         utc = UPGRADE_UTC + off[b];
         CHECK(play_and_save() == C0 + before[a] + off[b]);
         /* The file is now this release's: UTC and marked ... */
         CHECK(file_stamp() == utc && file_marked());
         /* ... loads on with no jump ... */
         utc += 5000;
         CHECK(play_and_save() == C0 + before[a] + off[b] + 5000);
         /* ... and reads right in a UTC emulator. */
         utc += 77;
         CHECK(utc_emulator_loads() == C0 + before[a] + off[b] + 5077);
      }
   /* Loading alone does not rewrite the file: until the game saves, the
    * old file stays as it was, and loads the same way every time. */
   utc = UPGRADE_UTC - 5000;
   CHECK(old_gbadhoc_saves(C0) == 0);
   utc = UPGRADE_UTC + 100;
   CHECK(boot() == 0);
   CHECK(fe_host_sram_flush(0) == 0);
   fe_host_shutdown();
   n = read_save();
   CHECK(n == (long)(RAM + 48) && !file_marked());
   CHECK(file_stamp() == UPGRADE_UTC - 5000 + zone && file_count() == C0);
   utc += 900;
   CHECK(play_and_save() == C0 + 6000);
   return 0;
}

/* Out to a UTC emulator and back.  The emulator's file has no mark, but it
 * was written after the upgrade, so it is UTC. */
static int test_round_trip(void)
{
   long long c;
   utc = UPGRADE_UTC + 2 * DAY;
   CHECK(utc_emulator_saves(C0, 48) == 0);
   CHECK(play_and_save() == C0);                  /* ours, marked */
   utc += 3600;
   c = utc_emulator_loads();                      /* theirs reads it */
   CHECK(c == C0 + 3600);
   utc += 1800;                                   /* played there */
   CHECK(utc_emulator_saves(c + 1800, 48) == 0);  /* theirs writes it */
   utc += 900;
   CHECK(play_and_save() == C0 + 3600 + 1800 + 900);   /* and back */
   utc += 60;
   CHECK(utc_emulator_loads() == C0 + 3600 + 1800 + 960);
   return 0;
}

/* The console's zone or daylight-saving setting changes between two
 * sessions.  A file of this release does not care: the cartridge counted
 * the seconds that passed.  (An old file follows the local clock once
 * more, as it always did.) */
static int test_zone_change(void)
{
   long long z = zone;
   utc = UPGRADE_UTC + 4 * DAY;
   CHECK(utc_emulator_saves(C0, 48) == 0);
   CHECK(play_and_save() == C0);
   zone = z + 3600;
   utc += 1000;
   CHECK(play_and_save() == C0 + 1000);
   zone = z - 7 * 3600;
   utc += 1000;
   CHECK(play_and_save() == C0 + 2000);
   zone = z;
   /* Old file, daylight saving switched on since it was written. */
   utc = UPGRADE_UTC - 4000;
   CHECK(old_gbadhoc_saves(C0) == 0);
   zone = z + 3600;
   utc = UPGRADE_UTC + 1000;
   CHECK(play_and_save() == C0 + 5000 + 3600);
   zone = z;
   return 0;
}

/* The one case no rule can get right, pinned so it cannot change unseen: a
 * UTC emulator's file from BEFORE the upgrade, imported after it.  It looks
 * exactly like an old GBAdhoc file and is read as 3.5.0 read every import:
 * off by the zone offset (or, where that makes the stamp "in the future",
 * with no time added). */
static int test_ambiguous(void)
{
   long long expect;
   utc = UPGRADE_UTC - 2 * DAY;
   CHECK(utc_emulator_saves(C0, 48) == 0);
   utc = UPGRADE_UTC + 3600;
   expect = 2 * DAY + 3600 + zone;
   CHECK(play_and_save() == C0 + (expect > 0 ? expect : 0));
   return 0;
}

/* A marked file is UTC even when its stamp is older than the upgrade (the
 * console's clock was wrong then, or CONFIG.INI was lost since). */
static int test_marked_before_upgrade(void)
{
   utc = UPGRADE_UTC - 6 * DAY;
   CHECK(write_save(C0, utc, 48, 1) == 0);
   utc = UPGRADE_UTC + 50;
   CHECK(play_and_save() == C0 + 6 * DAY + 50);
   return 0;
}

/* No old clock configured (every platform but the PSP, every host test):
 * all stamps are UTC.  And an old clock with no reading leaves the file to
 * the core, which keeps the registers. */
static int test_no_legacy(void)
{
   long long expect;
   with_legacy = 0;
   utc = UPGRADE_UTC - 5000;
   CHECK(utc_emulator_saves(C0, 48) == 0);
   utc = UPGRADE_UTC + 100;
   CHECK(play_and_save() == C0 + 5100);
   with_legacy = 1;

   utc = UPGRADE_UTC - 5000;
   CHECK(old_gbadhoc_saves(C0) == 0);
   legacy_reads = 0;
   utc = UPGRADE_UTC + 100;
   expect = 5100 - zone;              /* its local stamp, taken as UTC */
   CHECK(play_and_save() == C0 + (expect > 0 ? expect : 0));
   legacy_reads = 1;
   return 0;
}

/* Stamps from the future on either clock add nothing and reset nothing. */
static int test_future(void)
{
   utc = UPGRADE_UTC + 3 * DAY;
   CHECK(write_save(C0, utc + 20 * 3600, 48, 0) == 0);
   CHECK(play_and_save() == C0);
   CHECK(write_save(C0, utc + 20 * 3600, 48, 1) == 0);
   CHECK(play_and_save() == C0);
   CHECK(write_save(C0, 0x7fffffff7fffffffll, 48, 0) == 0);
   CHECK(play_and_save() == C0);
   CHECK(file_stamp() == utc && file_marked());
   return 0;
}

/* A save that is cartridge RAM and nothing else (a cartridge dump, a .srm
 * whose clock is in a separate .rtc).  The clock is unknown, so the game is
 * told: day carry set, which makes Pokemon G/S/C ask for the time.  Once --
 * the file is rewritten with a real trailer, and from then on the flag is
 * whatever the game left. */
static int write_raw(size_t n)
{
   FILE *f;
   unsigned i;
   int ok;
   for (i = 0; i < sizeof(file); i++) file[i] = (uint8_t)(i * 5 + 2);
   f = fopen(save_path, "wb");
   ok = f && fwrite(file, 1, n, f) == n;
   if (f) fclose(f);
   return ok ? 0 : -1;
}

static int test_bare_ram(void)
{
   static const size_t other[] = { RAM + 16, RAM + 47, RAM + 49, RAM + 64,
                                   RAM - 1, 0x2000 };
   unsigned i, k;
   utc = UPGRADE_UTC + 7 * DAY;
   /* Exactly the cartridge's RAM: flagged, from zero, and the first
    * periodic flush (not a forced one) writes the standard file. */
   CHECK(write_raw(RAM) == 0);
   CHECK(boot() == 0);
   utc += 12;
   CHECK(fe_host_sram_flush(0) == 1);
   fe_host_shutdown();
   CHECK(read_save() == (long)(RAM + 48));
   CHECK(file_flags() == 0x80 && file_count() == 12);
   CHECK(file_stamp() == utc && file_marked());
   for (i = 0; i < RAM; i++)
      CHECK(file[i] == (uint8_t)(i * 5 + 2));
   /* Next boot: the file has a trailer.  Nothing is added; the flag is
    * still there only because this "game" never cleared it ... */
   utc += 100;
   CHECK(play_and_save() == 112 && file_flags() == 0x80);
   /* ... and once the game has (the carry bit cleared in the file, as
    * its next save leaves it), no later boot brings it back. */
   file[RAM + 16] &= 0x7F;
   {
      FILE *f = fopen(save_path, "wb");
      CHECK(f && fwrite(file, 1, RAM + 48, f) == RAM + 48);
      fclose(f);
   }
   utc += 100;
   CHECK(play_and_save() == 212 && file_flags() == 0);
   utc += 100;
   CHECK(play_and_save() == 312 && file_flags() == 0);
   /* A UTC emulator reading the flagged file sees the flag too. */
   CHECK(write_raw(RAM) == 0);
   CHECK(play_and_save() == 0 && file_flags() == 0x80);
   CHECK((file[RAM + 16] & 0x80) && file[RAM + 17] == 0);

   /* No save file at all (a new game): no flag. */
   unlink(save_path);
   CHECK(play_and_save() == 0 && file_flags() == 0);
   /* A trailer of either size: no flag, on either clock. */
   CHECK(utc_emulator_saves(C0, 48) == 0);
   CHECK(play_and_save() == C0 && file_flags() == 0);
   CHECK(utc_emulator_saves(C0, 44) == 0);
   CHECK(play_and_save() == C0 && file_flags() == 0);
   utc = UPGRADE_UTC - 50;
   CHECK(old_gbadhoc_saves(C0) == 0);
   utc = UPGRADE_UTC + 7 * DAY;
   CHECK(play_and_save() == C0 + 7 * DAY + 50 && file_flags() == 0);
   /* Any other size: the RAM is kept and the clock starts from zero, as
    * before, with no flag. */
   for (k = 0; k < sizeof(other) / sizeof(other[0]); k++)
   {
      CHECK(write_raw(other[k]) == 0);
      CHECK(play_and_save() == 0 && file_flags() == 0);
   }
   return 0;
}

/* A cartridge with a battery and no clock: its file is its RAM, unmarked,
 * whatever is configured. */
static int test_no_clock_cart(void)
{
   char clock_rom[128], clock_save[160];
   unsigned i;
   FILE *f;
   memcpy(clock_rom, rom_path, sizeof(clock_rom));
   memcpy(clock_save, save_path, sizeof(clock_save));
   CHECK(make_rom(0x13) == 0);        /* MBC3+RAM+BATTERY */
   for (i = 0; i < RAM; i++) file[i] = (uint8_t)(i * 3 + 9);
   f = fopen(save_path, "wb");
   CHECK(f && fwrite(file, 1, RAM, f) == RAM);
   fclose(f);
   utc = UPGRADE_UTC + 10;
   CHECK(boot() == 0 && fe_host_sram_flush(1) == 1);
   fe_host_shutdown();
   CHECK(read_save() == (long)RAM);
   for (i = 0; i < RAM; i++)
      CHECK(file[i] == (uint8_t)(i * 3 + 9));
   unlink(save_path);
   unlink(rom_path);
   memcpy(rom_path, clock_rom, sizeof(rom_path));
   memcpy(save_path, clock_save, sizeof(save_path));
   return 0;
}

int main(void)
{
   static const long long zones[] = { -8 * 3600, -5 * 3600, -12600, 0, 3600,
                                      20700, 9 * 3600, 13 * 3600 };
   unsigned z;
   if (make_rom(0x10) != 0)           /* MBC3+TIMER+RAM+BATTERY */
   {
      fprintf(stderr, "FAIL: cannot write the test ROM\n");
      return 1;
   }
   for (z = 0; z < sizeof(zones) / sizeof(zones[0]); z++)
   {
      zone = zones[z];
      upgrade = UPGRADE_UTC + zone;   /* gb_rtc_utc_since: local time */
      if (test_export() || test_import(48) || test_import(44) ||
          test_old_save() || test_round_trip() || test_zone_change() ||
          test_ambiguous() || test_marked_before_upgrade() ||
          test_no_legacy() || test_future() || test_bare_ram() ||
          test_no_clock_cart())
         return 1;
   }
   unlink(save_path);
   unlink(rom_path);
   printf("gb rtc utc: export, import (48/44 B), 3.5.0 local-time saves, "
          "round trip, zone change, future stamps, RAM-only saves flagged once "
          "at %u zones (UTC-8 .. UTC+13)\n", z);
   return 0;
}

/* Stubs for libretro and wireless services: a GB boot never calls the GBA
 * core or starts networking, but fe_host.c contains both backends. */
void retro_set_environment(retro_environment_t cb) { (void)cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { (void)cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) { }
void retro_deinit(void) { }
bool retro_load_game(const struct retro_game_info *game) { (void)game; return false; }
void retro_get_system_av_info(struct retro_system_av_info *info) { (void)info; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
void retro_run(void) { }
void retro_reset(void) { }
void retro_unload_game(void) { }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size)
{ (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size)
{ (void)data; (void)size; return false; }
void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
void fe_np_gb_set_receive(fe_np_gb_receive_fn fn, void *u) { (void)fn; (void)u; }
void fe_np_gb_set_peer_callback(fe_np_gb_peer_fn fn, void *u) { (void)fn; (void)u; }
int fe_np_gb_peer_ready(uint8_t *id) { (void)id; return 0; }
int fe_np_gb_rx_how(void) { return 0; }
int fe_np_gb_send_room(void) { return 0; }
int fe_np_gb_unacked(void) { return 0; }
int fe_np_gb_peer_mac(uint8_t mac[6]) { (void)mac; return -1; }
int fe_np_gb_local_id(uint8_t *id) { (void)id; return 0; }
int fe_np_gb_send(uint16_t id, const void *p, size_t n)
{ (void)id; (void)p; (void)n; return -1; }
void fe_np_pump(void) { }
int fe_np_link_held(int id, uint32_t q) { (void)id; (void)q; return -1; }
