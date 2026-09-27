#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define function_cc
#include "../../cpu.h"

static void test_halt_wakes_without_irq_delivery(void)
{
  const u32 ie = IRQ_VBLANK;
  const u32 flags = IRQ_VBLANK;

  assert(cpu_halt_wake_requested(CPU_HALT, ie, flags));
  assert(!cpu_irq_delivery_enabled(ie, flags, 0, 0));
  assert(!cpu_irq_delivery_enabled(ie, flags, 1, 0x80));
}

static void test_irq_entry_requires_both_gates(void)
{
  const u32 ie = IRQ_TIMER0;
  const u32 flags = IRQ_TIMER0;

  assert(cpu_irq_delivery_enabled(ie, flags, 1, 0));
  assert(!cpu_irq_delivery_enabled(ie, flags, 0, 0));
  assert(!cpu_irq_delivery_enabled(ie, flags, 1, 0x80));
}

static void test_non_enabled_request_does_not_wake_halt(void)
{
  assert(!cpu_halt_wake_requested(CPU_HALT, IRQ_TIMER0, IRQ_VBLANK));
  assert(!cpu_halt_wake_requested(CPU_HALT, 0, IRQ_VBLANK));
  assert(!cpu_halt_wake_requested(CPU_STOP, IRQ_VBLANK, IRQ_VBLANK));
  assert(!cpu_irq_delivery_enabled(IRQ_TIMER0, IRQ_VBLANK, 1, 0));
}

int main(void)
{
  test_halt_wakes_without_irq_delivery();
  test_irq_entry_requires_both_gates();
  test_non_enabled_request_does_not_wake_halt();
  puts("PASS HALT wake and IRQ delivery policy");
  return 0;
}
