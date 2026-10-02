/* White-box test for the FF durability nudge (fe_host.c): a battery save made
 * during fast-forward is persisted by the io writer without waiting for FF to
 * end, and nothing at all happens when no save is pending.
 *
 * The writer thread is simulated: the fake io yield runs one writer pass
 * (fe_host_sram_service_io) when `writer_runs` is set, which is exactly what a
 * ~1 ms sleep on the emulation thread gives the real one.  With it clear, the
 * writer is starved -- the Unlimited-FF case the nudge exists for. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../frontend-common/fe_host.c"

/* The core's counter (gba_memory.c).  Defining it here satisfies the weak
 * reference in fe_host.c. */
volatile uint32_t backup_write_gen;

static uint8_t live_sram[FE_SRAM_SIZE];
static unsigned fake_wakes, fake_yields, writer_passes;
static int writer_runs = 1;
static uint64_t fake_now = 1000;

/* Mid-scan injection: when armed, the Nth CRC call of the NEXT writer pass
 * mutates live SRAM block `inject_blk` and bumps the generation, as a game
 * write racing the scan would. */
static int inject_at = -1, inject_blk, crc_calls;

static uint64_t fake_time_us(void) { return fake_now; }

static void fake_wake(void) { fake_wakes++; }
static void fake_yield(void)
{
   fake_yields++;
   fake_now += 1000;
   if (writer_runs)
   {
      crc_calls = 0;
      writer_passes++;
      (void)fe_host_sram_service_io();
      inject_at = -1;
   }
}
static const fe_host_io fake_io = { fake_wake, fake_yield };

void *retro_get_memory_data(unsigned id)
{
   assert(id == RETRO_MEMORY_SAVE_RAM);
   return live_sram;
}

uint32_t fe_crc32(uint32_t crc, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   size_t i;
   unsigned bit;
   if (inject_at >= 0 && crc_calls++ == inject_at)
   {
      live_sram[(size_t)inject_blk * SRAM_BLOCK + 17] ^= 0x5A;
      backup_write_gen++;
   }
   crc = ~crc;
   for (i = 0; i < len; i++)
   {
      crc ^= p[i];
      for (bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
   }
   return ~crc;
}

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }

size_t gbcore_save_ram_size(gbcore_t *core) { (void)core; return 0; }
size_t gbcore_cart_ram_size(gbcore_t *core) { (void)core; return 0; }
int gbcore_save_ram_read(gbcore_t *core, void *dst, size_t cap)
{ (void)core; (void)dst; (void)cap; return -1; }
/* A GB link session is never up in this test (gb_family is 0). */
gbdual_t *fe_gblink_dual(const fe_gblink *s) { (void)s; return NULL; }
int fe_gblink_local_slot(const fe_gblink *s) { (void)s; return 0; }
const gbcore_api_t *gbdual_api(int slot) { (void)slot; return NULL; }
gbcore_t *gbdual_core(const gbdual_t *d, int slot) { (void)d; (void)slot; return NULL; }

static const char *path = "/tmp/gpsp-sram-ff-nudge-test.sav";

/* One emulated frame followed by the main loop's nudge call. */
static int frame(int ff)
{
   frame_count++;
   fake_now += 16667;
   return fe_host_sram_ff_nudge(ff);
}

static unsigned frames(unsigned n, int ff)
{
   unsigned y = 0;
   while (n--)
      y += (unsigned)frame(ff);
   return y;
}

/* The game saves: mutate a block and bump the generation, as write_backup
 * does. */
static void game_write(unsigned blk, uint8_t v)
{
   live_sram[(size_t)blk * SRAM_BLOCK + 3] = v;
   backup_write_gen++;
}

static int disk_matches_live(void)
{
   uint8_t disk[FE_SRAM_SIZE];
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f)
      return 0;
   n = fread(disk, 1, sizeof(disk), f);
   fclose(f);
   return n == sizeof(disk) && memcmp(disk, live_sram, sizeof(disk)) == 0;
}

int main(void)
{
   FILE *f;
   unsigned y, i;

   memset(live_sram, 0x31, sizeof(live_sram));
   f = fopen(path, "wb");
   assert(f);
   assert(fwrite(live_sram, 1, sizeof(live_sram), f) == sizeof(live_sram));
   assert(fclose(f) == 0);

   backup_write_gen = 7;   /* whatever the game wrote before the load */
   host.save_path = path;
   host.time_us = fake_time_us;
   sram_load();
   assert(sram_have_blk && sram_disk_gen == 7 && !ffn_pending);
   fe_host_set_io(&fake_io);

   /* 1. Nothing pending: zero yields, zero wakes, in FF or out of it. */
   assert(frames(2000, 1) == 0 && frames(100, 0) == 0);
   assert(fake_yields == 0 && fake_wakes == 0);

   /* 2. A save outside FF: nothing changes -- no yield, no scan request.
    *    The writer's own 5 s cadence owns it, exactly as before. */
   game_write(4, 0xA1);
   assert(frames(200, 0) == 0);
   assert(fake_yields == 0 && sram_scan_req == 0 && ffn_pending);

   /* 3. FF starts with that save still unpersisted and long settled: the
    *    first FF frame nudges, the writer persists it, and it clears. */
   y = frames(1, 1);
   assert(y == 1 && writer_passes == 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(500, 1) == 0 && !ffn_pending);

   /* 4. A save made DURING FF waits SRAM_FF_SETTLE_FRAMES of quiet first,
    *    then is persisted by a single nudge. */
   game_write(9, 0xB2);
   y = frames(SRAM_FF_SETTLE_FRAMES - 1, 1);
   assert(y == 0);
   game_write(10, 0xB3);                 /* the save is still writing */
   y = frames(SRAM_FF_SETTLE_FRAMES, 1);
   assert(y == 0);                       /* settle restarts at the last write */
   y = frames(1, 1);
   assert(y == 1 && disk_matches_live());
   assert(frames(100, 1) == 0 && !ffn_pending);

   /* 5. A write racing the writer's scan must not be lost: the pass that
    *    started before it publishes only the generation it read, so the
    *    nudge keeps going until a later pass has seen the new bytes.
    *    Block 2 is CRC'd before the injection at CRC call 8 (block 4). */
   game_write(20, 0xC4);
   inject_at = 8;
   inject_blk = 2;
   y = frames(SRAM_FF_SETTLE_FRAMES + 1, 1);
   assert(y == 1);
   assert(sram_disk_gen != backup_write_gen);  /* raced write not credited */
   assert(!disk_matches_live());               /* ...and indeed not on disk */
   y = frames(SRAM_FF_SETTLE_FRAMES + 5, 1);
   assert(y == 1 && sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(100, 1) == 0 && !ffn_pending);

   /* 6. Starved writer: one yield per SRAM_FF_NUDGE_EVERY frames, bounded by
    *    SRAM_FF_NUDGE_CAP, then silence until the next save re-arms it. */
   writer_runs = 0;
   game_write(5, 0xD5);
   frames(SRAM_FF_SETTLE_FRAMES - 1, 1);
   y = frames(100, 1);
   assert(y == (100 + SRAM_FF_NUDGE_EVERY - 1) / SRAM_FF_NUDGE_EVERY);
   y += frames(SRAM_FF_NUDGE_CAP * SRAM_FF_NUDGE_EVERY * 2, 1);
   assert(y == SRAM_FF_NUDGE_CAP && ffn_gave_up);
   /* A new save re-arms it.  The OLDER save has now been pending for longer
    * than SRAM_FF_MAX_DEFER_FRAMES, so there is no settle wait: nudging
    * resumes on the very next frame, one yield per SRAM_FF_NUDGE_EVERY. */
   game_write(6, 0xD6);
   y = frames(SRAM_FF_SETTLE_FRAMES + 1, 1);
   assert(y == (SRAM_FF_SETTLE_FRAMES + SRAM_FF_NUDGE_EVERY) /
               SRAM_FF_NUDGE_EVERY && !ffn_gave_up);
   writer_runs = 1;
   frames(SRAM_FF_NUDGE_EVERY, 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(50, 1) == 0 && !ffn_pending);

   /* 7. A game that never stops writing is still persisted, after
    *    SRAM_FF_MAX_DEFER_FRAMES, while it keeps writing. */
   y = 0;
   for (i = 0; i < SRAM_FF_MAX_DEFER_FRAMES + 10; i++)
   {
      game_write(12, (uint8_t)i);
      y += (unsigned)frame(1);
   }
   assert(y > 0);

   /* 8. An I/O failure is never reported as persisted. */
   frames(SRAM_FF_SETTLE_FRAMES * 2, 1);
   assert(!ffn_pending);
   host.save_path = "/tmp/gpsp-sram-ff-missing-dir/save.sav";
   sram_close();                         /* force a reopen on the bad path */
   game_write(15, 0xE7);
   y = frames(SRAM_FF_SETTLE_FRAMES + 20, 1);
   assert(y > 0 && sram_disk_gen != backup_write_gen && ffn_pending);
   host.save_path = path;
   frames(SRAM_FF_NUDGE_EVERY * 2, 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());

   /* 9. Disabled (the harness A/B arm): never yields, still tracks. */
   fe_host_sram_ff_nudge_enable(0);
   y = fake_yields;
   game_write(3, 0xF8);
   assert(frames(1000, 1) == 0 && fake_yields == y && ffn_pending);
   fe_host_sram_ff_nudge_enable(1);

   /* 10. No writer thread installed (synchronous path): a no-op. */
   fe_host_set_io(NULL);
   assert(frames(1000, 1) == 0 && fake_yields == y);

   sram_close();
   unlink(path);
   puts("PASS sram FF nudge: idle-free, settles, persists, survives a racing "
        "write, bounded when starved, never credits a failed write");
   return 0;
}
