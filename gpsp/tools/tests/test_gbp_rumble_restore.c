/* White-box test of the real rumble accumulator and GB Player state. */
#include <assert.h>
#include <math.h>

/* Keep the production rumble and GBP implementations in this test TU so the
 * linker can discard unrelated emulator sections. */
#include "../../gba_memory.c"
#include "../../gbp.c"

u32 cpu_ticks;
u32 reg[64];

int main(void)
{
  float duty;

  /* Restoring GBP OFF after an old run was rumbling must discard the old
   * frame-local timestamp, even when the restored CPU clock moved backward. */
  cpu_ticks = 2000000;
  write_rumble(false, true);
  cpu_ticks = 1000;
  gbp_set_state(0);
  rumble_restore_state((gbp_get_state() & 0x10) != 0);
  assert(rumble_active_pct() == 0.0f);

  /* OFF -> ON restoration must start at the restored CPU tick, not inherit a
   * stale timestamp from the previous timeline. */
  gbp_set_state(0x10);
  rumble_restore_state((gbp_get_state() & 0x10) != 0);
  cpu_ticks += 10000;
  duty = rumble_active_pct();
  assert(duty > 0.0f && duty < 1.0f);

  /* ON -> OFF rebases to silence without unsigned underflow after rewind. */
  cpu_ticks = 250;
  gbp_set_state(0);
  rumble_restore_state((gbp_get_state() & 0x10) != 0);
  assert(rumble_active_pct() == 0.0f);

  /* GPIO rumble is also represented in the shared accumulator, even when the
   * GBP latch is off. The post-load rebase must preserve that restored pin. */
  rumble_enabled = true;
  gpio_regs[0] = 0x08;
  rumble_restore_state(false);
  cpu_ticks += 10000;
  duty = rumble_active_pct();
  assert(duty > 0.0f && duty < 1.0f);

  gpio_regs[0] = 0;
  rumble_restore_state(false);
  assert(rumble_active_pct() == 0.0f);
  return 0;
}
