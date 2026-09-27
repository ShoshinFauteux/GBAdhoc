/* Regression coverage for malformed variable-length CodeBreaker payloads. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common.h"
#include "../cheats.h"
#include "../gba_memory.h"

static unsigned writes;
u16 io_registers[512];
u32 read_memory16(u32 address) { (void)address; return 0; }
u32 read_memory32(u32 address) { (void)address; return 0; }
cpu_alert_type write_memory8(u32 address, u8 value) { (void)address; (void)value; writes++; return 0; }
cpu_alert_type write_memory16(u32 address, u16 value) { (void)address; (void)value; writes++; return 0; }
cpu_alert_type write_memory32(u32 address, u32 value) { (void)address; (void)value; writes++; return 0; }

int main(void)
{
   char oversized[1025];
   /* value=4 means eight payload bytes, requiring two six-byte records.
    * Supplying one must be rejected without activating or executing it. */
   cheat_clear();
   assert(cheat_parse(0, "50001000 0004\n12345678 9abc") == CheatErrorNotSupported);
   process_cheats();
   assert(writes == 0);

   /* A complete two-record payload remains accepted and executes eight byte
    * writes, guarding the valid boundary adjacent to the malformed case. */
   cheat_clear();
   assert(cheat_parse(0,
      "50001000 0004\n12345678 9abc\ndef01234 5678") == CheatNoError);
   process_cheats();
   assert(writes == 8);

   /* Opcode 4 is also variable-length and must not leave a missing parameter
    * record behind as an executable opcode. */
   cheat_clear();
   assert(cheat_parse(1, "40002000 000a") == CheatErrorNotSupported);
   process_cheats();
   assert(writes == 8);

   /* A complete slide record still writes the requested number of values. */
   cheat_clear();
   assert(cheat_parse(1, "40002000 000a\n00020003 0004") == CheatNoError);
   process_cheats();
   assert(writes == 11);

   /* An oversized replacement clears the old active cheat as well. */
   memset(oversized, '0', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   assert(cheat_parse(1, oversized) == CheatErrorTooBig);
   process_cheats();
   assert(writes == 11);

   /* A conditional that skips "the next code" skips a whole super code.  Its
    * payload record here reads as opcode 8 (16-bit write); skipping only the
    * header used to execute it.  The trailing 8-bit write still runs. */
   cheat_clear();
   writes = 0;
   assert(cheat_parse(2, "A0001000 0000\n50001000 0003\n83000000 1234\n"
                         "30000100 0001") == CheatNoError);
   process_cheats();
   assert(writes == 1);

   /* Payload bytes are data: a record whose top nibble is 9 does not make
    * the cheat "encrypted" (it was rejected as such before). */
   cheat_clear();
   writes = 0;
   assert(cheat_parse(2, "50001000 0003\n9ABCDEF0 1234") == CheatNoError);
   process_cheats();
   assert(writes == 6);
   puts("PASS CodeBreaker opcodes 4/5: truncated payloads rejected, "
        "payload records never run as opcodes");
   return 0;
}
