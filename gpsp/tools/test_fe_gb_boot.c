/* Production frontend-path smoke test using tiny generated GB/CGB ROMs. */
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

#include "../frontend-common/fe_host.h"
#include "../frontend-common/netpacket_host.h"
#include "../libretro/libretro-common/include/libretro.h"

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct stats
{
   unsigned video_calls;
   unsigned audio_frames;
   unsigned width, height;
   size_t pitch;
   uint64_t clock;
   uint32_t frame_hash;       /* FNV-1a of the last presented frame */
   int saw_rom_loaded;
} stats_t;
static stats_t *active_stats;
static const char *active_console_name;

static const uint8_t logo[48] = {
   0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
   0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
   0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
   0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
};

static void make_rom_typed(uint8_t rom[0x8000], int color, uint8_t cart_type,
                           uint8_t ram_size_code);
static void make_rom(uint8_t rom[0x8000], int color)
{
   make_rom_typed(rom, color, 0, 0);
}

static void make_rom_typed(uint8_t rom[0x8000], int color, uint8_t cart_type,
                           uint8_t ram_size_code)
{
   unsigned i, sum = 0;
   uint8_t check = 0;
   memset(rom, 0, 0x8000);
   rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
   memcpy(rom + 0x104, logo, sizeof(logo));
   memcpy(rom + 0x134, "GBADHOC TEST", 12);
   rom[0x143] = color ? 0x80 : 0;
   rom[0x147] = cart_type;
   rom[0x148] = 0;
   rom[0x149] = ram_size_code;
   rom[0x14A] = 1;
   for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - rom[i] - 1);
   rom[0x14D] = check;
   for (i = 0; i < 0x8000; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
   rom[0x14E] = (uint8_t)(sum >> 8);
   rom[0x14F] = (uint8_t)sum;
   /* Enable LCD, write SB, start an external (DMG) or internal (CGB) byte,
    * then idle. The production core must reach and render this ROM. */
   rom[0x150] = 0x31; rom[0x151] = 0xFE; rom[0x152] = 0xFF;
   rom[0x153] = 0x3E; rom[0x154] = 0x91;
   rom[0x155] = 0xE0; rom[0x156] = 0x40;
   rom[0x157] = 0x3E; rom[0x158] = 0x3C;
   rom[0x159] = 0xE0; rom[0x15A] = 0x01;
   rom[0x15B] = 0x3E; rom[0x15C] = color ? 0x81 : 0x80;
   rom[0x15D] = 0xE0; rom[0x15E] = 0x02;
   rom[0x15F] = 0x18; rom[0x160] = 0xFE;
}

static void video(const uint16_t *p, unsigned w, unsigned h, size_t pitch)
{
   stats_t *s = active_stats;
   unsigned x, y;
   if (!p)
      return;
   s->video_calls++; s->width = w; s->height = h; s->pitch = pitch;
   s->frame_hash = 2166136261u;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         s->frame_hash = (s->frame_hash ^
                          p[y * (pitch / 2) + x]) * 16777619u;
}
static void audio(const int16_t *p, size_t n)
{ stats_t *s = active_stats; if (p || !n) s->audio_frames += (unsigned)n; }
static uint64_t time_us(void)
{ active_stats->clock += 16683; return active_stats->clock; }
static uint32_t input(void) { return 0; }

static int run_one(fe_console_t console, int color)
{
   uint8_t rom[0x8000];
   char path[] = "/tmp/gbadhoctestXXXXXX";
   char rom_path[128];
   int fd = mkstemp(path);
   stats_t stats = {0};
   fe_host_config cfg;
   CHECK(fd >= 0);
   make_rom(rom, color);
   CHECK(write(fd, rom, sizeof(rom)) == (ssize_t)sizeof(rom));
   close(fd);
   snprintf(rom_path, sizeof(rom_path), "%s.%s", path, color ? "gbc" : "gb");
   CHECK(rename(path, rom_path) == 0);
   memset(&cfg, 0, sizeof(cfg));
   cfg.console = console;
   cfg.rom_path = rom_path;
   cfg.video_frame = video;
   cfg.audio_frames = audio;
   cfg.input_bitmask = input;
   cfg.time_us = time_us;
   active_stats = &stats;
   active_console_name = color ? "GBC" : "GB";
   CHECK(fe_host_boot(&cfg) == 0);
   CHECK(stats.saw_rom_loaded);
   for (unsigned i = 0; i < 3; i++) fe_host_run_frame();
   CHECK(fe_host_frame_count() == 3);
   /* The core's own frame is handed on without a copy: 256-pixel rows. */
   CHECK(stats.video_calls == 3 && stats.width == 160 && stats.height == 144 &&
         stats.pitch == 512);
   CHECK(fe_host_last_frame(NULL) != NULL);
   fe_host_shutdown();
   unlink(rom_path);
   CHECK(access(rom_path, F_OK) != 0);
   printf("frontend launch console=%s frames=%u video=%ux%u\n",
          color ? "GBC" : "GB", stats.video_calls, stats.width, stats.height);
   return 0;
}

/* ---- battery save with a cartridge clock (MBC3+TIMER+RAM+BATTERY) ---- */
#define RTC_RAM 0x8000u            /* header RAM code 3 = 32 KiB */
#define RTC_TRAILER 48u            /* the VBA-M/BGB 64-bit form, as SameBoy
                                    * also wrote it */
static time_t fake_wall = 1700000000;
static time_t wallclock(void) { return fake_wall; }

static long file_size(const char *path)
{
   long n;
   FILE *f = fopen(path, "rb");
   if (!f) return -1;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fclose(f);
   return n;
}

static int run_rtc_save(void)
{
   static uint8_t rom[0x8000], ram[RTC_RAM], disk[RTC_RAM + RTC_TRAILER];
   char path[] = "/tmp/gbadhocrtcXXXXXX";
   char rom_path[128], save_path[160];
   stats_t stats = {0};
   fe_host_config cfg;
   FILE *f;
   unsigned i;
   uint64_t stamp = 0;
   int fd = mkstemp(path);
   CHECK(fd >= 0);
   make_rom_typed(rom, 1, 0x10, 3);
   CHECK(write(fd, rom, sizeof(rom)) == (ssize_t)sizeof(rom));
   close(fd);
   snprintf(rom_path, sizeof(rom_path), "%s.gbc", path);
   snprintf(save_path, sizeof(save_path), "%s.gbc.sav", path);
   CHECK(rename(path, rom_path) == 0);

   /* A RAM-only save, as several other emulators write it.  It must be
    * loaded -- the old exact-size check skipped it, and the first periodic
    * flush then replaced it with a blank cartridge. */
   for (i = 0; i < RTC_RAM; i++) ram[i] = (uint8_t)(i * 7 + 3);
   f = fopen(save_path, "wb");
   CHECK(f && fwrite(ram, 1, sizeof(ram), f) == sizeof(ram));
   fclose(f);

   memset(&cfg, 0, sizeof(cfg));
   cfg.console = FE_CONSOLE_GBC;
   cfg.rom_path = rom_path;
   cfg.save_path = save_path;
   cfg.video_frame = video;
   cfg.audio_frames = audio;
   cfg.input_bitmask = input;
   cfg.time_us = time_us;
   cfg.wallclock = wallclock;
   active_stats = &stats;
   active_console_name = "GBC";
   CHECK(fe_host_boot(&cfg) == 0);
   for (i = 0; i < 3; i++) fe_host_run_frame();

   /* Foreign layout: rewritten once in the standard one (RAM + 48-byte
    * trailer), with the loaded RAM. */
   CHECK(fe_host_sram_flush(0) == 1);
   CHECK(file_size(save_path) == (long)(RTC_RAM + RTC_TRAILER));
   f = fopen(save_path, "rb");
   CHECK(f && fread(disk, 1, sizeof(disk), f) == sizeof(disk));
   fclose(f);
   CHECK(memcmp(disk, ram, RTC_RAM) == 0);
   /* The clock trailer is stamped with the frontend's wall clock, not
    * time() (which is uptime on a PSP). */
   for (i = 0; i < 8; i++)
      stamp |= (uint64_t)disk[RTC_RAM + 40 + i] << (8 * i);
   CHECK(stamp == (uint64_t)fake_wall);

   /* Time passing is not a change: the clock trailer must not make the
    * periodic flush rewrite an untouched cartridge every 5 s. */
   fake_wall += 30;
   CHECK(fe_host_sram_flush(0) == 0);
   fe_host_shutdown();
   CHECK(file_size(save_path) == (long)(RTC_RAM + RTC_TRAILER));

   unlink(save_path);
   unlink(rom_path);
   printf("frontend rtc save: ram-only import kept, clock stamped, "
          "idle flush skipped\n");
   return 0;
}

static int write_file(const char *path, const void *data, size_t n)
{
   FILE *f = fopen(path, "wb");
   int ok = f && fwrite(data, 1, n, f) == n;
   if (f) fclose(f);
   return ok ? 0 : -1;
}

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

/* A save written by the previous (SameBoy) core or VBA-M/BGB: cart RAM plus
 * the clock trailer.  The clock must resume where it was plus the time the
 * console was off; the RAM must survive; a 44-byte (32-bit stamp) trailer
 * works too; and a stamp from the future resets the clock. */
static int run_old_save_import(unsigned trailer, int future)
{
   static uint8_t rom[0x8000], ram[RTC_RAM], img[RTC_RAM + RTC_TRAILER],
                  disk[RTC_RAM + RTC_TRAILER];
   char path[] = "/tmp/gbadhocoldXXXXXX";
   char rom_path[128], save_path[160];
   stats_t stats = {0};
   fe_host_config cfg;
   FILE *f;
   unsigned i;
   int fd = mkstemp(path);
   const uint8_t *t;
   CHECK(fd >= 0);
   make_rom_typed(rom, 1, 0x10, 3);
   CHECK(write(fd, rom, sizeof(rom)) == (ssize_t)sizeof(rom));
   close(fd);
   snprintf(rom_path, sizeof(rom_path), "%s.gbc", path);
   snprintf(save_path, sizeof(save_path), "%s.gbc.sav", path);
   CHECK(rename(path, rom_path) == 0);

   for (i = 0; i < RTC_RAM; i++) ram[i] = (uint8_t)(i * 13 + 5);
   memcpy(img, ram, RTC_RAM);
   memset(img + RTC_RAM, 0, RTC_TRAILER);
   /* live: day 5, 03:02:10; latched: day 1, 00:00:07 */
   put32(img + RTC_RAM + 0, 10); put32(img + RTC_RAM + 4, 2);
   put32(img + RTC_RAM + 8, 3);  put32(img + RTC_RAM + 12, 5);
   put32(img + RTC_RAM + 16, 0);
   put32(img + RTC_RAM + 20, 7); put32(img + RTC_RAM + 32, 1);
   put32(img + RTC_RAM + 40,
         (uint32_t)(future ? fake_wall + 1000 : fake_wall - 100));
   CHECK(write_file(save_path, img, RTC_RAM + trailer) == 0);

   memset(&cfg, 0, sizeof(cfg));
   cfg.console = FE_CONSOLE_GBC;
   cfg.rom_path = rom_path;
   cfg.save_path = save_path;
   cfg.video_frame = video;
   cfg.audio_frames = audio;
   cfg.input_bitmask = input;
   cfg.time_us = time_us;
   cfg.wallclock = wallclock;
   active_stats = &stats;
   active_console_name = "GBC";
   CHECK(fe_host_boot(&cfg) == 0);
   fe_host_run_frame();
   CHECK(fe_host_sram_flush(1) == 1);
   fe_host_shutdown();

   CHECK(file_size(save_path) == (long)(RTC_RAM + RTC_TRAILER));
   f = fopen(save_path, "rb");
   CHECK(f && fread(disk, 1, sizeof(disk), f) == sizeof(disk));
   fclose(f);
   CHECK(memcmp(disk, ram, RTC_RAM) == 0);
   t = disk + RTC_RAM;
   if (future)
   {
      /* Not real clock data: the clock restarts from zero. */
      CHECK(get32(t) == 0 && get32(t + 4) == 0 && get32(t + 8) == 0 &&
            get32(t + 12) == 0);
   }
   else
   {
      /* 03:02:10 + 100 s = 03:03:50, day 5 */
      CHECK(get32(t) == 50 && get32(t + 4) == 3 && get32(t + 8) == 3 &&
            get32(t + 12) == 5 && (get32(t + 16) & 1) == 0);
      CHECK(get32(t + 20) == 7 && get32(t + 32) == 1);
   }
   CHECK(get32(t + 40) == (uint32_t)fake_wall && get32(t + 44) == 0);
   unlink(save_path);
   unlink(rom_path);
   printf("frontend old save import: trailer=%u %s\n", trailer,
          future ? "future stamp -> clock reset" : "clock advanced 100 s");
   return 0;
}

/* Save states: a DMG ROM that rewrites BGP as fast as it can, so every
 * frame's image depends on exact CPU timing.  Frames after a load must be
 * bit-identical to the frames after the save; a damaged image or one for
 * another cartridge must be refused without disturbing the game. */
static int run_state_roundtrip(void)
{
   static uint8_t rom[0x8000], blob[512 * 1024];
   char path[] = "/tmp/gbadhocstXXXXXX";
   char rom_path[128], st_path[160], bad_path[160];
   uint32_t after_save[20];
   stats_t stats = {0};
   fe_host_config cfg;
   unsigned i;
   long n;
   FILE *f;
   int fd = mkstemp(path);
   CHECK(fd >= 0);
   make_rom(rom, 0);
   /* LD SP / LCD on as make_rom does, then: loop: INC A; LDH (47),A; JR loop */
   rom[0x159] = 0x3C; rom[0x15A] = 0xE0; rom[0x15B] = 0x47;
   rom[0x15C] = 0x18; rom[0x15D] = 0xFB;
   CHECK(write(fd, rom, sizeof(rom)) == (ssize_t)sizeof(rom));
   close(fd);
   snprintf(rom_path, sizeof(rom_path), "%s.gb", path);
   snprintf(st_path, sizeof(st_path), "%s.gb.st0", path);
   snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
   CHECK(rename(path, rom_path) == 0);

   memset(&cfg, 0, sizeof(cfg));
   cfg.console = FE_CONSOLE_GB;
   cfg.rom_path = rom_path;
   cfg.video_frame = video;
   cfg.audio_frames = audio;
   cfg.input_bitmask = input;
   cfg.time_us = time_us;
   active_stats = &stats;
   active_console_name = "GB";
   CHECK(fe_host_boot(&cfg) == 0);
   for (i = 0; i < 30; i++) fe_host_run_frame();
   CHECK(fe_host_state_save(st_path) == 0);
   for (i = 0; i < 20; i++)
   {
      fe_host_run_frame();
      after_save[i] = stats.frame_hash;
   }
   CHECK(after_save[0] != after_save[1] || after_save[1] != after_save[2]);
   CHECK(fe_host_state_load(st_path) == 0);
   for (i = 0; i < 20; i++)
   {
      fe_host_run_frame();
      CHECK(stats.frame_hash == after_save[i]);
   }

   /* Truncated, and one byte of the cartridge identity changed. */
   f = fopen(st_path, "rb");
   CHECK(f);
   n = (long)fread(blob, 1, sizeof(blob), f);
   fclose(f);
   CHECK(n > 64);
   CHECK(write_file(bad_path, blob, (size_t)n - 1) == 0);
   CHECK(fe_host_state_load(bad_path) != 0);
   blob[20] ^= 0x01;
   CHECK(write_file(bad_path, blob, (size_t)n) == 0);
   CHECK(fe_host_state_load(bad_path) != 0);
   /* The refused loads left the game running where it was. */
   fe_host_run_frame();
   fe_host_shutdown();
   unlink(bad_path);
   unlink(st_path);
   unlink(rom_path);
   printf("frontend save state: %ld bytes, 20 frames identical after load, "
          "damaged images refused\n", n);
   return 0;
}

/* Stubs for libretro and wireless services: GB boot never calls the GBA core
 * or starts networking, but fe_host.c contains both production backends. */
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
void fe_evt(const char *fmt, ...)
{
   char message[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(message, sizeof(message), fmt, ap);
   va_end(ap);
   if (active_stats && strstr(message, "gb_rom_loaded console=") &&
       strstr(message, active_console_name) &&
       strstr(message, "title=GBADHOC TEST"))
      active_stats->saw_rom_loaded = 1;
}
void fe_log(const char *fmt, ...) { (void)fmt; }
void fe_np_gb_set_receive(fe_np_gb_receive_fn fn, void *u) { (void)fn; (void)u; }
void fe_np_gb_set_peer_callback(fe_np_gb_peer_fn fn, void *u) { (void)fn; (void)u; }
int fe_np_gb_peer_ready(uint8_t *id) { (void)id; return 0; }
int fe_np_gb_local_id(uint8_t *id) { (void)id; return 0; }
int fe_np_gb_send(uint16_t id, const void *p, size_t n)
{ (void)id; (void)p; (void)n; return -1; }
void fe_np_pump(void) { }

int main(void)
{
   return run_one(FE_CONSOLE_GB, 0) || run_one(FE_CONSOLE_GBC, 1) ||
          run_rtc_save() || run_old_save_import(RTC_TRAILER, 0) ||
          run_old_save_import(44, 0) || run_old_save_import(RTC_TRAILER, 1) ||
          run_state_roundtrip();
}
