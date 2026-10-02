/* Focused white-box regressions for the GBA cartridge RTC serial protocol. */
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
u32 frame_counter;
u32 reg[64];
u32 cph_bkr, cph_bkw;
u16 palette_ram[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
u16 io_registers[512];
u8 vram[1024 * 96];
u8 vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];

static time_t fixed_clock(void)
{
  return (time_t)1704114309; /* 2024-01-01 13:05:09 UTC */
}

static void gpio(u8 value)
{
  write_gpio(0xC4, value);
}

static void begin_command(void)
{
  gpio(0x00); /* CS low resets protocol */
  gpio(0x04); /* CS high, clock low */
}

static void send_command(u8 command)
{
  int bit;
  for (bit = 7; bit >= 0; --bit)
  {
    u8 pins = 0x04 | (((command >> bit) & 1) << 1);
    gpio(pins);
    gpio(pins | 1);
  }
}

static void send_data_lsb_first(u8 data)
{
  unsigned bit;
  for (bit = 0; bit < 8; ++bit)
  {
    u8 pins = 0x04 | (((data >> bit) & 1) << 1);
    gpio(pins);
    gpio(pins | 1);
  }
}

static void end_transaction(void)
{
  gpio(0x00);
}

int main(void)
{
  rtc_enabled = true;
  gpio_regs[1] = 0x07; /* data, clock, and chip select are outputs */
  gpsp_wallclock = fixed_clock;
  rtc_base_time = fixed_clock();
  frame_counter = 0;

  /* RESET consumes the command only and clears the control register. */
  rtc_status = 0x22;
  begin_command();
  send_command(RTC_COMMAND_RESET);
  assert(rtc_state == RTC_IDLE);
  assert(rtc_status == 0);
  end_transaction();

  /* The status byte is serialised LSB-first: 0x40 stays 0x40. */
  begin_command();
  send_command(RTC_COMMAND_WRITE_STATUS);
  send_data_lsb_first(0x40);
  assert(rtc_status == 0x40);
  end_transaction();

  /* Status bit 6 set selects 24-hour output: 13:05:09. */
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME);
  assert((u8)rtc_data == 0x13);
  assert((u8)(rtc_data >> 8) == 0x05);
  assert((u8)(rtc_data >> 16) == 0x09);
  end_transaction();

  /* Status bit 6 clear selects 12-hour output: 1 PM has PM in bit 7. */
  rtc_status = 0x00;
  begin_command();
  send_command(RTC_COMMAND_WRITE_STATUS);
  send_data_lsb_first(0x00);
  end_transaction();
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME);
  assert((u8)rtc_data == 0x81);
  assert((u8)(rtc_data >> 8) == 0x05);
  assert((u8)(rtc_data >> 16) == 0x09);
  end_transaction();

  /* The full-time command uses the same 12-hour encoding for its hour byte. */
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME_FULL);
  assert((u8)(rtc_data >> 32) == 0x81);
  assert((u8)(rtc_data >> 40) == 0x05);
  assert((u8)(rtc_data >> 48) == 0x09);
  end_transaction();

  /* 01:05 AM is 0x01 (no PM bit); noon is 12 PM, or 0x92. */
  rtc_base_time = (s64)1704071109; /* 2024-01-01 01:05:09 UTC */
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME);
  assert((u8)rtc_data == 0x01);
  end_transaction();
  rtc_base_time = (s64)1704110409; /* 2024-01-01 12:00:09 UTC */
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME);
  assert((u8)rtc_data == 0x92);
  end_transaction();

  /* Return to 24-hour mode for an unchanged-format control case. */
  rtc_status = 0x40;
  begin_command();
  send_command(RTC_COMMAND_WRITE_STATUS);
  send_data_lsb_first(0x40);
  end_transaction();
  rtc_base_time = (s64)1704114309;
  begin_command();
  send_command(RTC_COMMAND_OUTPUT_TIME);
  assert((u8)rtc_data == 0x13);
  assert((u8)(rtc_data >> 8) == 0x05);
  assert((u8)(rtc_data >> 16) == 0x09);
  end_transaction();

  return 0;
}
