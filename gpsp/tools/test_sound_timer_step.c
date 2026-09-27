#include <assert.h>
#include <stdio.h>

#include "../common.h"
#include "../cpu.h"
#include "../gba_memory.h"
#include "../sound.h"

/* Include the production implementation so this focused check exercises the
 * actual inactive-channel fast path.  --gc-sections drops unrelated sound
 * entry points and their frontend dependencies. */
dma_transfer_type dma[DMA_CHAN_CNT];
static u32 dma_calls;
cpu_alert_type dma_transfer(unsigned dma_chan, int *cycles)
{
   (void)dma_chan;
   (void)cycles;
   dma_calls++;
   return CPU_ALERT_NONE;
}

#include "../sound.c"

static void reference_silent_advance(u32 step, u32 *fractional,
                                     u32 *buffer_index)
{
   while(*fractional <= 0xFFFFFF)
   {
      *fractional += step;
      *buffer_index = (*buffer_index + 2) % BUFFER_SIZE;
   }
   *fractional = fp8_24_fractional_part(*fractional);
   *buffer_index &= BUFFER_SIZE_MASK;
}

static void check_inactive_fast_forward(u32 step, u32 fractional,
                                        u32 initial_index, u32 enable_sound)
{
   direct_sound_struct *ds = &direct_sound_channel[0];
   u32 expected_index = initial_index;
   u32 expected_fractional = fractional;
   u32 i;

   reference_silent_advance(step, &expected_fractional, &expected_index);
   for(i = 0; i < 32; i++)
      ds->fifo[i] = (s8)(i * 3 - 40);
   ds->fifo_base = 7;
   ds->fifo_top = 28; /* Keep the post-pop occupancy above the DMA threshold. */
   ds->fifo_fractional = fractional;
   ds->buffer_index = initial_index;
   ds->status = enable_sound ? DIRECT_SOUND_INACTIVE : DIRECT_SOUND_LEFTRIGHT;
   ds->volume_halve = 0;
   sound_on = enable_sound;
   memset(sound_buffer, 0x5A, sizeof(sound_buffer));

   assert(sound_timer(step, 0) == 0);
   assert(ds->fifo_base == 8);
   assert(ds->fifo_top == 28);
   assert(ds->fifo_fractional == expected_fractional);
   assert(ds->buffer_index == expected_index);
   assert(dma_calls == 0);
   for(i = 0; i < BUFFER_SIZE; i++)
      assert(sound_buffer[i] == (s32)0x5A5A5A5A);
   for(i = 0; i < 32; i++)
      assert(ds->fifo[i] == (s8)(i * 3 - 40));
}

/* Exhaustive over every step the fast path accepts (1..0x10000) at the phase
 * extremes and a spread between them: the closed form must land exactly where
 * the loop does, and the loop's iteration count is what the silent path would
 * otherwise have cost.  Index/phase only -- the full buffer scan above is too
 * slow to repeat 65536 x N times. */
static void sweep_inactive_fast_forward(void)
{
   direct_sound_struct *ds = &direct_sound_channel[0];
   u32 step, k, checked = 0;

   sound_on = 1;
   for(step = 1; step <= 0x10000; step++)
   {
      const u32 phases[4] = { 0, step / 2, step - 1, (step * 7u) / 11u };
      for(k = 0; k < 4; k++)
      {
         u32 fr = phases[k], idx = (step * 2654435761u) & BUFFER_SIZE_MASK & ~1u;
         u32 exp_fr = fr, exp_idx = idx;
         reference_silent_advance(step, &exp_fr, &exp_idx);
         ds->fifo_base = 0;
         ds->fifo_top = 28;
         ds->fifo_fractional = fr;
         ds->buffer_index = idx;
         ds->status = DIRECT_SOUND_INACTIVE;
         assert(sound_timer(step, 0) == 0);
         assert(ds->fifo_fractional == exp_fr);
         assert(ds->buffer_index == exp_idx);
         checked++;
      }
   }
   printf("sweep: %u inactive fast-path cases match the loop\n", checked);
}

int main(void)
{
   const u32 rate = GBA_SOUND_FREQUENCY;
   assert(direct_sound_timer_step(0, rate) == 0);
   assert(direct_sound_timer_step(256, rate) == 0x01000000u);
   /* reload=1 used to calculate 2^32, truncate to zero, and hang the mixer. */
   assert(direct_sound_timer_step(1, rate) == 0xFF000000u);
   assert(direct_sound_timer_step(256, 0) == 0);
   /* A zero step must not enter the sample-advance loop or mutate the FIFO. */
   direct_sound_channel[0].fifo_base = 7;
   direct_sound_channel[0].fifo_fractional = 0x123456u;
   direct_sound_channel[0].buffer_index = 234;
   assert(sound_timer(0, 0) == 0);
   assert(direct_sound_channel[0].fifo_base == 7);
   assert(direct_sound_channel[0].fifo_fractional == 0x123456u);
   assert(direct_sound_channel[0].buffer_index == 234);
   check_inactive_fast_forward(64, 0, BUFFER_SIZE - 4, 0);
   check_inactive_fast_forward(0x10000, 17, 1234, 1);
   /* If a rate change leaves phase >= step, retain the original short loop. */
   check_inactive_fast_forward(64, 0xFFFFC0, BUFFER_SIZE - 2, 1);
   sweep_inactive_fast_forward();
   puts("PASS timer-derived audio steps stay nonzero and accumulator-safe");
   return 0;
}
