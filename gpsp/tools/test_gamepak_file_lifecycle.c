#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the implementation so this focused host test can exercise the
 * real static raw-ROM loader without exporting its internal entry point. */
#include "../gba_memory.c"

struct RFILE
{
  int id;
  int64_t size;
  int64_t position;
  int closed;
  int read_error;
};

static struct RFILE fake_files[6];
static unsigned open_attempts;
static unsigned close_count;
u8 *memory_map_read[8 * 1024];

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
  (void)mode;
  (void)hints;
  open_attempts++;
  if (strcmp(path, "missing.gba") == 0)
    return NULL;
  if (strcmp(path, "A.gba") == 0)
  {
    fake_files[0] = (struct RFILE){ 0, 0x8000, 0, 0, 0 };
    return (RFILE *)&fake_files[0];
  }
  if (strcmp(path, "B.gba") == 0)
  {
    fake_files[1] = (struct RFILE){ 1, 0x8000, 0, 0, 0 };
    return (RFILE *)&fake_files[1];
  }
  if (strcmp(path, "read-error-chunk.gba") == 0)
  {
    fake_files[2] = (struct RFILE){ 2, 0x8000, 0, 0, 1 };
    return (RFILE *)&fake_files[2];
  }
  if (strcmp(path, "read-error-mini.gba") == 0)
  {
    fake_files[3] = (struct RFILE){ 3, 0x100000, 0, 0, 1 };
    return (RFILE *)&fake_files[3];
  }
  if (strcmp(path, "oversized.gba") == 0)
  {
    fake_files[4] = (struct RFILE){ 4, 0x04000001, 0, 0, 0 };
    return (RFILE *)&fake_files[4];
  }
  if (strcmp(path, "short-bios.bin") == 0)
  {
    fake_files[5] = (struct RFILE){ 5, 0x2000, 0, 0, 0 };
    return (RFILE *)&fake_files[5];
  }
  return NULL;
}

int64_t filestream_get_size(RFILE *stream)
{
  return ((struct RFILE *)stream)->size;
}

int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{
  (void)seek_position;
  ((struct RFILE *)stream)->position = offset;
  return 0;
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
  struct RFILE *file = (struct RFILE *)stream;
  if (file->read_error)
    return -1;
  int64_t remaining = file->size - file->position;
  int64_t count = len < remaining ? len : remaining;
  memset(data, 0xA0 + file->id, (size_t)count);
  file->position += count;
  return count;
}

int filestream_close(RFILE *stream)
{
  struct RFILE *file = (struct RFILE *)stream;
  assert(!file->closed);
  file->closed = 1;
  close_count++;
  return 0;
}

int main(void)
{
  unsigned i;

  /* Keep the test's allocation small while retaining the production page
   * size and LRU metadata shape. */
  gamepak_buffer_count = 1;
  gamepak_buffers[0] = (u8 *)malloc(gamepak_buffer_blocksize);
  assert(gamepak_buffers[0]);
  for (i = 0; i < 1024; i++)
  {
    gamepak_blk_queue[i].next_lru = (u16)(i + 1);
    gamepak_blk_queue[i].phy_rom = -1;
  }
  gamepak_lru_head = 0;
  gamepak_lru_tail = 31;

  assert(load_gamepak_raw("A.gba") == 0);
  assert(open_attempts == 1 && close_count == 0);
  assert(gamepak_file_large == (RFILE *)&fake_files[0]);

  /* Simulate a Memory Stick handle that goes stale after load (for example,
   * across PSP sleep). A page fault must close it, reopen by ROM path, and
   * recover the requested bytes instead of mapping a failed read. */
  fake_files[0].read_error = 1;
  {
    u8 *page = load_gamepak_page(0);
    assert(page != NULL);
    assert(page[0] == 0xA0 && page[32767] == 0xA0);
  }
  assert(open_attempts == 2 && close_count == 1);
  assert(gamepak_file_large == (RFILE *)&fake_files[0]);

  memory_unload_gamepak();
  assert(close_count == 2 && gamepak_file_large == NULL);

  assert(load_gamepak_raw("B.gba") == 0);
  assert(open_attempts == 3 && close_count == 2);
  assert(gamepak_file_large == (RFILE *)&fake_files[1]);

  /* A failed replacement load must first close B; otherwise it would leave
   * the previous content's file handle reachable after the failure. */
  assert(load_gamepak_raw("missing.gba") != 0);
  assert(open_attempts == 4 && close_count == 3);
  assert(gamepak_file_large == NULL);

  /* A signed read failure must not wrap to UINT32_MAX and masquerade as a
   * full read, either in the chunked loader or the 1 MiB mirror path. */
  assert(load_gamepak_raw("read-error-chunk.gba") != 0);
  assert(close_count == 4 && gamepak_file_large == NULL);
  assert(load_gamepak_raw("read-error-mini.gba") != 0);
  assert(close_count == 5 && gamepak_file_large == NULL);
  assert(gamepak_mini_rom == NULL);

  /* The map_rom_entry layout has only 64 MiB available in its third
   * cartridge window. A larger file must be rejected before any read/map. */
  assert(load_gamepak_raw("oversized.gba") != 0);
  assert(open_attempts == 7 && close_count == 6);
  assert(gamepak_file_large == NULL);
  for (i = 0x8000000u >> 15; i < 0xD000000u >> 15; i++)
    assert(memory_map_read[i] == NULL);

  /* Allocation failure must not report an empty ROM as successfully loaded. */
  memory_term();
  assert(gamepak_buffer_count == 0);
  assert(load_gamepak_raw("A.gba") != 0);
  assert(open_attempts == 7 && close_count == 6);

  assert(load_bios("short-bios.bin") != 0);
  assert(open_attempts == 8 && close_count == 7);

  memory_unload_gamepak();
  assert(close_count == 7);
  memory_term();
  puts("PASS ROM/BIOS file lifecycle rejects failed, oversized, empty-cache and truncated loads");
  return 0;
}
