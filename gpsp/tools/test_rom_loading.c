/* Extracted production load_gamepak_raw: compare pre-change and current
 * bytes/mappings/I/O, including small ROMs and a cache smaller than the ROM. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef struct { int64_t size, pos; } RFILE;
#define RETRO_VFS_FILE_ACCESS_READ 1
#define RETRO_VFS_FILE_ACCESS_HINT_NONE 0
static RFILE file;
static RFILE *gamepak_file_large;
static u8 *gamepak_buffers[32], *gamepak_mini_rom;
static u32 gamepak_buffer_count, gamepak_size, gamepak_file_blocks;
static const unsigned gamepak_buffer_blocksize = 1024*1024;
static bool gamepak_mirror_1m, gamepak_mini_materialized;
static int64_t gamepak_file_bytes;
static char gamepak_path[1024];
static struct { int phy_rom; } gamepak_blk_queue[1024];
static unsigned evict_at, maps, io_calls, notifications, last_done, expected_total;
static unsigned mapped_pages[1024], mapped_blocks[1024];
static int mini_fail;
static void (*gpsp_rom_load_progress)(u32, u32);
/* The extracted loader's production pre-load teardown is covered by the
 * dedicated gamepak lifecycle test; this fixture isolates byte loading. */
static void memory_unload_gamepak(void) {}
static void *rom_malloc(size_t size) { return mini_fail ? NULL : malloc(size); }
static RFILE *filestream_open(const char *name, int access, int hint)
{ (void)name; (void)access; (void)hint; file.pos=0; return &file; }
static int64_t filestream_get_size(RFILE *f) { return f->size; }
static void filestream_close(RFILE *f) { (void)f; }
static void filestream_seek(RFILE *f, int offset, int origin)
{ assert(origin == SEEK_SET); f->pos=offset; }
static int64_t filestream_read(RFILE *f, u8 *out, unsigned size)
{
   int64_t n = f->size - f->pos;
   unsigned i;
   if (n > size) n = size;
   for (i=0; i<(unsigned)n; i++) out[i]=(u8)((f->pos+i)*37u+11u);
   f->pos += n;
   io_calls++;
   return n;
}
#define map_null(type, start, end) ((void)0)
static void record_map(unsigned phyn, u8 *ptr, unsigned blocks)
{
   assert(ptr && maps < 1024);
   mapped_pages[maps]=phyn; mapped_blocks[maps++]=blocks;
}
#define map_rom_entry(type, phyn, ptr, blocks) record_map(phyn, ptr, blocks)
static unsigned evict_gamepak_page(void) { return evict_at++; }
static void update_gpio_romregs(void) {}
static void observer(u32 done, u32 total)
{
   assert(total == expected_total && done <= total && done >= last_done);
   last_done=done; notifications++;
}
#define malloc rom_malloc
#include "load_before.inc"
#include "load_after.inc"
#include "rom_scan_after.inc"
#undef malloc

static void reset(unsigned buffers, unsigned size, int fail)
{
   unsigned i;
   free(gamepak_mini_rom); gamepak_mini_rom=NULL;
   for(i=0;i<32;i++) { free(gamepak_buffers[i]); gamepak_buffers[i]=NULL; }
   for(i=0;i<buffers;i++) { gamepak_buffers[i]=malloc(1024*1024); assert(gamepak_buffers[i]); memset(gamepak_buffers[i],0,1024*1024); }
   gamepak_buffer_count=buffers; file.size=size; mini_fail=fail;
   gamepak_mini_materialized=gamepak_mirror_1m=false;
   gamepak_size=gamepak_file_blocks=0;
   evict_at=maps=io_calls=notifications=last_done=0;
   memset(gamepak_blk_queue,0,sizeof(gamepak_blk_queue));
   memset(mapped_pages,0,sizeof(mapped_pages)); memset(mapped_blocks,0,sizeof(mapped_blocks));
   expected_total=buffers*1024*1024;
   if(expected_total>size) expected_total=size;
}
static uint32_t fingerprint(void)
{
   uint32_t crc=2166136261u;
   unsigned b,i;
   for(b=0;b<gamepak_buffer_count;b++)
      for(i=0;i<1024*1024;i++) crc=(crc^gamepak_buffers[b][i])*16777619u;
   if(gamepak_mini_rom)
      for(i=0;i<4*1024*1024;i++) crc=(crc^gamepak_mini_rom[i])*16777619u;
   for(i=0;i<1024;i++) crc=(crc^mapped_pages[i]^mapped_blocks[i]^gamepak_blk_queue[i].phy_rom)*16777619u;
   return crc;
}
int main(void)
{
   const unsigned sizes[]={32768,1048576,2097289,16*1048576,32*1048576};
   const unsigned caches[]={1,6,32};
   unsigned s,c,fail;
   for(s=0;s<5;s++) for(c=0;c<3;c++) for(fail=0;fail<2;fail++)
   {
      uint32_t hash; unsigned calls, size, blocks, count; bool mini;
      reset(caches[c],sizes[s],fail); gpsp_rom_load_progress=NULL;
      assert(load_gamepak_raw_before("test.gba")==0);
      hash=fingerprint(); calls=io_calls; size=gamepak_size; blocks=gamepak_file_blocks; count=maps; mini=gamepak_mini_materialized;
      reset(caches[c],sizes[s],fail); gpsp_rom_load_progress=observer;
      assert(load_gamepak_raw_after("test.gba")==0);
      assert(fingerprint()==hash && io_calls==calls && gamepak_size==size && gamepak_file_blocks==blocks && maps==count && gamepak_mini_materialized==mini);
   assert(notifications>=2 && last_done==expected_total);
   }

   /* One shared scan must find every marker, including unaligned placements
    * that the former four-to-five independent scans had to rediscover. */
   reset(2, 2*1024*1024, 0);
   memset(gamepak_buffers[0], 0xFF, 1024*1024);
   memcpy(gamepak_buffers[0]+101, "EEPROM_V", 8);
   memcpy(gamepak_buffers[0]+203, "SRAM_V", 6);
   memcpy(gamepak_buffers[0]+307, "FLASH1M_V", 9);
   memcpy(gamepak_buffers[0]+409, "FLASH512_V", 10);
   assert(rom_scan_signatures(gamepak_buffers[0], 1024*1024) ==
          (ROM_SIG_EEPROM | ROM_SIG_SRAM | ROM_SIG_FLASH1M | ROM_SIG_FLASH5));

   /* If the first resident MiB has no marker, the fallback starts at the
    * next resident block and still finds later ROM backup metadata. */
   memset(gamepak_buffers[0], 0xFF, 1024*1024);
   memset(gamepak_buffers[1], 0xFF, 1024*1024);
   memcpy(gamepak_buffers[1]+4, "FLASH_V", 7);
   gamepak_size = 2*1024*1024;
   assert(rom_scan_signatures(gamepak_buffers[0], 1024*1024) == 0);
   assert(rom_scan_signatures_in_memory() == ROM_SIG_FLASH5);
   puts("PASS 30 before/after ROM byte/mapping/I/O comparisons; progress monotonic and exact");
   puts("PASS one-pass aligned/unaligned backup detection and later-block fallback");
   reset(1,0,0); gpsp_rom_load_progress=observer;
   assert(load_gamepak_raw_after("empty.gba")==-1 && notifications==0);
   file.size=0x20000001;
   assert(load_gamepak_raw_after("oversized.gba")==-1 && notifications==0);
   reset(0,0,0);
   puts("PASS empty/oversized rejection; partial last block; 1 MiB mirror allocation/fallback; constrained cache");
   return 0;
}
