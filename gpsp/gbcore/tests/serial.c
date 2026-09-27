/* Core-side cable test: the serial hook the ad-hoc link rides on.
 *
 * A generated MBC1+RAM+BATTERY ROM loads 0x5A into SB, starts a transfer
 * with the internal or the external clock, waits for SC bit 7 to clear,
 * then stores SB, a done marker (0x42) and IF into cartridge RAM, which the
 * test reads back through the battery image.  Covered:
 *   - the callback sees the outgoing byte and the clock mode once per byte
 *   - internal clock + peer: the peer's byte lands, with the interrupt flag
 *   - internal clock, no cable: the transfer still completes, with 0xFF
 *     (MasterBoy never completed it at all)
 *   - external clock + peer: the byte lands at once
 *   - external clock, no cable: the transfer waits, as on hardware
 *   - the internal-clock byte arrives after eight bit times, not at once
 */
#include "../gbcore.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
  int answer;          /* 1 = a peer answers, 0 = no cable */
  unsigned calls;
  uint8_t outgoing;
  int internal;
} cable_t;

static int on_serial(void *userdata, uint8_t outgoing, int internal_clock,
                     uint8_t *received)
{
  cable_t *c = (cable_t *)userdata;
  c->calls++;
  c->outgoing = outgoing;
  c->internal = internal_clock;
  if (!c->answer)
    return 0;
  *received = 0xA5;
  return 1;
}

static void make_rom(unsigned char rom[0x8000], int internal, int color)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  static const unsigned char program[] = {
    0x31, 0xFE, 0xFF,       /* 0150 LD SP,$FFFE          */
    0x3E, 0x0A,             /* 0153 LD A,$0A             */
    0xEA, 0x00, 0x00,       /* 0155 LD ($0000),A  RAM on */
    0x3E, 0x5A,             /* 0158 LD A,$5A             */
    0xE0, 0x01,             /* 015A LDH (SB),A           */
    0x3E, 0x00,             /* 015C LD A,start (patched) */
    0xE0, 0x02,             /* 015E LDH (SC),A           */
    0xF0, 0x02,             /* 0160 LDH A,(SC)           */
    0xCB, 0x7F,             /* 0162 BIT 7,A              */
    0x20, 0xFA,             /* 0164 JR NZ,$0160          */
    0xF0, 0x01,             /* 0166 LDH A,(SB)           */
    0xEA, 0x00, 0xA0,       /* 0168 LD ($A000),A         */
    0x3E, 0x42,             /* 016B LD A,$42             */
    0xEA, 0x01, 0xA0,       /* 016D LD ($A001),A         */
    0xF0, 0x0F,             /* 0170 LDH A,(IF)           */
    0xEA, 0x02, 0xA0,       /* 0172 LD ($A002),A         */
    0x18, 0xFE              /* 0175 JR $0175             */
  };
  unsigned i;
  unsigned char check = 0;
  memset(rom, 0, 0x8000);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "SERIAL TEST", 11);
  rom[0x143] = color ? 0x80 : 0x00;
  rom[0x147] = 0x03;                  /* MBC1+RAM+BATTERY */
  rom[0x149] = 0x02;                  /* 8 KiB */
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  memcpy(rom + 0x150, program, sizeof(program));
  rom[0x15D] = internal ? 0x81 : 0x80;
}

static int run(int internal, int answer, int color)
{
  unsigned char rom[0x8000], ram[0x2000];
  cable_t cable = {0};
  gbcore_callbacks_t cb = {0};
  gbcore_t *core;
  unsigned f;
  make_rom(rom, internal, color);
  cable.answer = answer;
  cb.userdata = &cable;
  cb.serial_transfer = on_serial;
  core = gbcore_create(NULL, rom, sizeof(rom),
                       color ? FE_CONSOLE_GBC : FE_CONSOLE_GB, 32768, &cb);
  CHECK(core != NULL);
  CHECK(gbcore_save_ram_size(core) == sizeof(ram));
  for (f = 0; f < 10; f++)
    CHECK(gbcore_run_frame(core, 0) == 0);
  CHECK(gbcore_save_ram_read(core, ram, sizeof(ram)) == 0);
  gbcore_shutdown(core);

  CHECK(cable.calls == 1);
  CHECK(cable.outgoing == 0x5A);
  CHECK(cable.internal == internal);
  if (!internal && !answer)
  {
    CHECK(ram[1] != 0x42);           /* still waiting for a clock */
  }
  else
  {
    CHECK(ram[1] == 0x42);
    CHECK(ram[0] == (answer ? 0xA5 : 0xFF));
    CHECK(ram[2] & 0x08);            /* IF: serial */
  }
  printf("serial %s clock, %s: SB=%02x done=%s\n",
         internal ? "internal" : "external",
         answer ? "peer" : "no cable", ram[0],
         ram[1] == 0x42 ? "yes" : "waiting");
  return 0;
}

/* Eight bit times at 8192 Hz are 4096 clocks, about 0.23 of a frame.  With
 * the ROM above the transfer is started early in the first frame, so after
 * that frame it has completed -- but the byte must not have landed
 * synchronously with the SC write either: SB still reads the outgoing
 * value if the ROM samples it immediately (checked via a second ROM that
 * reads SB right after starting). */
static int run_timing(void)
{
  unsigned char rom[0x8000], ram[0x2000];
  cable_t cable = {0};
  gbcore_callbacks_t cb = {0};
  gbcore_t *core;
  make_rom(rom, 1, 0);
  /* Replace the wait loop with an immediate read of SB. */
  rom[0x160] = 0x00; rom[0x161] = 0x00;   /* NOP NOP */
  rom[0x162] = 0x00; rom[0x163] = 0x00;
  rom[0x164] = 0x00; rom[0x165] = 0x00;
  cable.answer = 1;
  cb.userdata = &cable;
  cb.serial_transfer = on_serial;
  core = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GB, 32768, &cb);
  CHECK(core != NULL);
  CHECK(gbcore_run_frame(core, 0) == 0);
  CHECK(gbcore_save_ram_read(core, ram, sizeof(ram)) == 0);
  gbcore_shutdown(core);
  CHECK(ram[1] == 0x42);
  CHECK(ram[0] == 0x5A);                  /* not yet shifted in */
  printf("serial internal clock: byte not delivered before the bit times\n");
  return 0;
}

int main(void)
{
  return run(1, 1, 0) || run(1, 0, 0) || run(0, 1, 0) || run(0, 0, 0) ||
         run(1, 1, 1) || run_timing();
}
