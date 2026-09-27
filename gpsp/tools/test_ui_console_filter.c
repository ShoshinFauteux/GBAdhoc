#include <assert.h>
#include "../psp/ui_psp.h"

int main(void)
{
   assert(ui_rom_matches_console("Pokemon Red.gb", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Red.GB", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Blue.gB", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("Pokemon Crystal.gbc", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Crystal.gbc", FE_CONSOLE_GBC));
   assert(ui_rom_matches_console("Pokemon Crystal.GbC", FE_CONSOLE_GBC));
   assert(ui_rom_matches_console("Pokemon Emerald.gba", FE_CONSOLE_GBA));
   assert(ui_rom_matches_console("Pokemon Emerald.GbA", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("Pokemon Emerald.GBA", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("folder.gb/rom.gbc", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("extensionless", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console(NULL, FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("game.gb", (fe_console_t)99));
   assert(ui_rom_stem_length("Pokemon Red.gb") == 11);
   assert(ui_rom_stem_length("Pokemon Crystal.gbc") == 15);
   assert(ui_rom_stem_length("dir.with.dot/Pokemon Emerald.gba") == 28);
   return 0;
}
