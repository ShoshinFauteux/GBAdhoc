#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include production code so this test exercises the exact span planner used
 * by dma_transfer and its legacy cycle accounting, without requiring a PSP. */
#include "../../gba_memory.c"

u32 cpu_ticks;
u32 frame_counter;
u32 execute_cycles;
u32 reg[64];
u32 cph_dman;
u32 cph_bkr;
u32 cph_bkw;
u32 sound_frequency = 1;
u32 sound_freq_bits = 0;
u32 sound_on;
timer_type timer[4];
direct_sound_struct direct_sound_channel[2];
gbc_sound_struct gbc_sound_channel[4];
u32 gbc_sound_master_volume_left;
u32 gbc_sound_master_volume_right;
u32 gbc_sound_master_volume;
u32 gbc_sound_buffer_index;
u32 gbc_sound_last_cpu_ticks;
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
s32 affine_reference_x[2];
s32 affine_reference_y[2];
static unsigned dma_irq_count;

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{ (void)path; (void)mode; (void)hints; return NULL; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{ (void)stream; (void)offset; (void)seek_position; return -1; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{ (void)stream; (void)data; (void)len; return -1; }
int filestream_close(RFILE *stream)
{ (void)stream; return 0; }

void smc_prof_note_iwram(u32 offset) { (void)offset; }
void smc_prof_note_ewram(u32 offset) { (void)offset; }
cpu_alert_type flag_interrupt(irq_type irq_raised)
{ (void)irq_raised; dma_irq_count++; return CPU_ALERT_NONE; }
cpu_alert_type check_interrupt(void) { return CPU_ALERT_NONE; }
cpu_alert_type write_siocnt(u16 value)
{ (void)value; return CPU_ALERT_NONE; }
cpu_alert_type write_rcnt(u16 value)
{ (void)value; return CPU_ALERT_NONE; }
void sound_timer_queue32(u32 channel, u32 value)
{ (void)channel; (void)value; }
void sound_reset_fifo(u32 channel) { (void)channel; }
void render_gbc_sound(void) {}

static void assert_plan(u32 src, u32 dst, u32 remaining, u32 transfer_bytes,
  u32 src_direction, u32 dst_direction, const u32 *expected, u32 expected_count)
{
  u32 i;
  for (i = 0; i < expected_count; i++)
  {
    u32 span = dma_transfer_region_span(src, dst, remaining, transfer_bytes,
      src_direction, dst_direction);
    assert(span == expected[i]);
    src += span * transfer_bytes * dma_stride[src_direction];
    dst += span * transfer_bytes * dma_stride[dst_direction];
    remaining -= span;
  }
  assert(remaining == 0);
}

static void prepare_dma(u32 src, u32 dst, u32 length,
  u32 src_direction, u32 dst_direction, u32 length_type)
{
  memset(&dma[0], 0, sizeof(dma[0]));
  memset(io_registers, 0, sizeof(io_registers));
  dma[0].source_address = src;
  dma[0].dest_address = dst;
  dma[0].length = length;
  dma[0].source_direction = src_direction;
  dma[0].dest_direction = dst_direction;
  dma[0].length_type = length_type;
  dma[0].repeat_type = DMA_NO_REPEAT;
  dma[0].start_type = DMA_START_IMMEDIATELY;
  io_registers[REG_DMA0CNT_H] = 0x8000;
}

static void test_cycles_use_start_regions(void)
{
  int cycles;

  /* Destination's last pointer advances to region 16 (0x10000000), which is
   * outside def_seq_cycles[0..15]. The sampled cycle row must remain region 15. */
  prepare_dma(0x02000000, 0x0FFFFFFE, 1,
    DMA_FIXED, DMA_INCREMENT, DMA_16BIT);
  cycles = 0;
  assert(dma_transfer(0, &cycles) == CPU_ALERT_NONE);
  assert(dma[0].dest_address == 0x10000000);
  assert(cycles == (int)(def_seq_cycles[2][0] + def_seq_cycles[15][0]));

  /* The source wraps through zero to 0xFFFFFFFC (top byte 255) after two
   * decrementing halfwords. Its cycle row must still be the original row 0. */
  prepare_dma(0x00000000, 0x02000000, 2,
    DMA_DECREMENT, DMA_FIXED, DMA_16BIT);
  dma_bus_val = 0xBEEF;
  cycles = 0;
  assert(dma_transfer(0, &cycles) == CPU_ALERT_NONE);
  assert(dma[0].source_address == 0xFFFFFFFC);
  assert(cycles == (int)(2 * (def_seq_cycles[0][0] + def_seq_cycles[2][0])));
  assert(*(u16 *)ewram == 0xBEEF);
}

static void test_transfer_copies_across_source_boundary(void)
{
  int cycles = 0;
  u16 first = 0x1234;
  u16 second = 0xABCD;

  memset(ewram, 0, sizeof(ewram));
  memset(iwram, 0, sizeof(iwram));
  memcpy(ewram + 0x3FFFE, &first, sizeof(first));
  memcpy(iwram + 0x8000, &second, sizeof(second));

  /* First halfword reads the last EWRAM address; the next reads from IWRAM.
   * Both are written to a separate IWRAM range so the second source remains
   * unchanged. This executes the production copy dispatcher across the map
   * boundary, rather than only checking the planner's span lengths. */
  prepare_dma(0x02FFFFFE, 0x03000004, 2,
    DMA_INCREMENT, DMA_INCREMENT, DMA_16BIT);
  dma[0].irq = 1;
  dma_irq_count = 0;
  assert(dma_transfer(0, &cycles) == CPU_ALERT_NONE);

  assert(*(u16 *)(iwram + 0x8004) == first);
  assert(*(u16 *)(iwram + 0x8006) == second);
  assert(dma_irq_count == 1);
  assert(dma[0].source_address == 0x03000002);
  assert(dma[0].dest_address == 0x03000008);
}

static void test_transfer_copies_across_destination_boundary(void)
{
  int cycles = 0;
  u16 value = 0xC0DE;

  memset(ewram, 0, sizeof(ewram));
  memset(vram, 0, sizeof(vram));
  memset(oam_ram, 0, sizeof(oam_ram));
  memcpy(ewram, &value, sizeof(value));

  /* The destination moves from the last VRAM halfword into OAM while the
   * source remains fixed in EWRAM. Both writes must use their own dispatcher. */
  prepare_dma(0x02000000, 0x06FFFFFE, 2,
    DMA_FIXED, DMA_INCREMENT, DMA_16BIT);
  assert(dma_transfer(0, &cycles) == CPU_ALERT_NONE);

  assert(*(u16 *)(vram + 0x17FFE) == value);
  assert(oam_ram[0] == value);
  assert(dma[0].dest_address == 0x07000002);
}

int main(void)
{
  static const u32 paired_inc[] = { 1, 1 };
  static const u32 paired_dec[] = { 1, 1 };
  static const u32 staggered_inc[] = { 1, 1, 1 };
  static const u32 same_mapping[] = { 2 };

  /* Reported regression: EWRAM->IWRAM and VRAM->OAM on the same halfword. */
  assert_plan(0x02FFFFFE, 0x06FFFFFE, 2, 2,
    DMA_INCREMENT, DMA_INCREMENT, paired_inc, 2);

  /* Decrement crosses out of IWRAM/OAM at address zero.  The first transfer
   * at the boundary belongs to the upper region; the next belongs below it. */
  assert_plan(0x03000000, 0x07000000, 2, 2,
    DMA_DECREMENT, DMA_DECREMENT, paired_dec, 2);

  /* Source crosses after two transfers, destination after one. */
  assert_plan(0x02FFFFFC, 0x06FFFFFE, 3, 2,
    DMA_INCREMENT, DMA_INCREMENT, staggered_inc, 3);

  /* 0x08 and 0x09 are distinct address windows with the same DMA dispatch;
   * crossing between them should not create a spurious split. */
  assert_plan(0x08FFFFFE, 0x02000000, 2, 2,
    DMA_INCREMENT, DMA_FIXED, same_mapping, 1);

  test_cycles_use_start_regions();
  test_transfer_copies_across_source_boundary();
  test_transfer_copies_across_destination_boundary();

  puts("PASS DMA region splitting and start-region cycle accounting");
  return 0;
}
