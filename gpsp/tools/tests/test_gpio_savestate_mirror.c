/* Verify that loading GPIO state refreshes the cartridge ROM-register mirror. */
#include <assert.h>
#include <stdlib.h>

#include "../../gba_memory.c"

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{ (void)path; (void)mode; (void)hints; return NULL; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{ (void)stream; (void)offset; (void)seek_position; return -1; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{ (void)stream; (void)data; (void)len; return -1; }
int filestream_close(RFILE *stream)
{ (void)stream; return 0; }

u32 cpu_ticks;
u32 reg[64];
u32 cph_bkr, cph_bkw;
u16 palette_ram[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
u16 io_registers[512];
u8 vram[1024 * 96];
u8 vram_clean[VRAM_DIRTY_PAGES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];

int main(void)
{
  static u8 rom_page[0x8000];
  static u8 state[512 * 1024];
  u8 *header = state;
  unsigned state_size;

  rtc_enabled = true;
  gamepak_size = sizeof(rom_page);
  memory_map_read[0x08000000 >> 15] = rom_page;
  gpio_regs[0] = 0x0005;
  gpio_regs[1] = 0x0007;
  gpio_regs[2] = 0x0001;
  update_gpio_romregs();
  assert(read_memory16(0x080000C4) == 0x0005);
  assert(read_memory16(0x080000C6) == 0x0007);
  assert(read_memory16(0x080000C8) == 0x0001);

  state_size = memory_write_savestate(state + 4);
  assert(state_size > 0 && state_size + 4 < sizeof(state));
  bson_write_u32(header, state_size + 4);

  /* Model the GPIO registers and ROM mirror changing after the save. */
  gpio_regs[0] = 0x0002;
  gpio_regs[1] = 0x0003;
  gpio_regs[2] = 0x0001;
  update_gpio_romregs();
  assert(read_memory16(0x080000C4) == 0x0002);
  assert(read_memory16(0x080000C6) == 0x0003);

  assert(memory_read_savestate(state));
  assert(read_memory16(0x080000C4) == 0x0005);
  assert(read_memory16(0x080000C6) == 0x0007);
  assert(read_memory16(0x080000C8) == 0x0001);
  return 0;
}
