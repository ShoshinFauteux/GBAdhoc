/* White-box regression test for recovering a block left WRITING when the
 * asynchronous SRAM writer is stopped before completing its file operation. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../frontend-common/fe_host.c"

static uint8_t live_sram[FE_SRAM_SIZE];
static unsigned fake_wakes, fake_yields;

static void fake_wake(void) { fake_wakes++; }
static void fake_yield(void) { fake_yields++; }
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

/* fe_host.c also carries the GB backend.  This test never boots it (gb_core
 * stays NULL), but the SRAM entry points it calls branch to it, so the
 * linker needs the symbols. */
size_t gbcore_save_ram_size(gbcore_t *core) { (void)core; return 0; }
size_t gbcore_cart_ram_size(gbcore_t *core) { (void)core; return 0; }
int gbcore_save_ram_read(gbcore_t *core, void *dst, size_t cap)
{ (void)core; (void)dst; (void)cap; return -1; }

int main(void)
{
   const char *path = "/tmp/gpsp-sram-interrupted-test.sav";
   const char *bad_path = "/tmp/gpsp-sram-missing-dir/save.sav";
   uint8_t disk_sram[FE_SRAM_SIZE];
   FILE *f;
   unsigned i, interrupted = 7;

   memset(disk_sram, 0x31, sizeof(disk_sram));
   memcpy(live_sram, disk_sram, sizeof(live_sram));
   memset(live_sram + interrupted * SRAM_BLOCK, 0xA7, SRAM_BLOCK);

   f = fopen(path, "wb");
   assert(f);
   assert(fwrite(disk_sram, 1, sizeof(disk_sram), f) == sizeof(disk_sram));
   assert(fclose(f) == 0);

   host.save_path = path;
   sram_have_blk = 1;
   sram_have_crc = 1;
   sram_full_req = 0;
   sram_scan_req = 0;
   sram_fp = NULL;
   memset((void *)sram_blk_state, SBLK_CLEAN, sizeof(sram_blk_state));
   for (i = 0; i < SRAM_BLOCKS; i++)
      sram_blk_crc[i] = fe_crc32(0, disk_sram + i * SRAM_BLOCK, SRAM_BLOCK);

   /* Simulate the writer being interrupted after claiming the block but
    * before its staged bytes reached disk. */
   sram_blk_state[interrupted] = SBLK_WRITING;
   assert(sram_pending() == 1);

   /* While async ownership still exists, sync must only request/yield; it
    * must not reclaim the writer's state or race a live SRAM mutation. */
   fe_host_set_io(&fake_io);
   fe_host_sram_sync();
   assert(fake_wakes == 1 && fake_yields > 0);
   assert(sram_blk_state[interrupted] == SBLK_WRITING);
   memset(live_sram + interrupted * SRAM_BLOCK, 0xC4, SRAM_BLOCK);

   /* io_thread_stop() joins/terminates the writer before clearing this hook. */
   fe_host_set_io(NULL);
   host.save_path = bad_path;
   fe_host_sram_sync();
   assert(sram_full_req == 1); /* failed delta I/O is queued as a full retry */
   fe_host_sram_sync();
   assert(sram_full_req == 1); /* failed full retry is not cleared either */

   /* Restore a writable destination. The retained request must save the
    * current live image even though the whole-image SRAM CRC did not change. */
   host.save_path = path;
   fe_host_sram_sync();

   assert(sram_pending() == 0);
   assert(sram_full_req == 0);
   assert(sram_blk_state[interrupted] == SBLK_CLEAN);
   assert(sram_blk_crc[interrupted] ==
          fe_crc32(0, live_sram + interrupted * SRAM_BLOCK, SRAM_BLOCK));
   f = fopen(path, "rb");
   assert(f);
   assert(fread(disk_sram, 1, sizeof(disk_sram), f) == sizeof(disk_sram));
   assert(fclose(f) == 0);
   assert(memcmp(disk_sram, live_sram, sizeof(live_sram)) == 0);

   sram_close();
   unlink(path);
   puts("PASS interrupted WRITING block is retried, persisted, and cleared");
   return 0;
}
