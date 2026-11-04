/* xw_cli.c -- xmbwrap on the command line (issue #29, docs/XMB-LAUNCH.md).
 *
 *   xmbwrap --rom-file G:/PSP/GAME/GBAdhoc/roms/Game.gba [options]
 *   xmbwrap --root G:/ --rom Game.gba [options]
 *
 *   --title T        XMB title (default: the ROM's name)
 *   --icon IMG       icon art (default: GBAdhoc's boxart for the ROM, if any)
 *   --background IMG background art (default: GBAdhoc's hero art, if any)
 *   --no-art         no icon or background
 *   --gbadhoc DIR    GBAdhoc's folder (default: inferred, or PSP/GAME/GBAdhoc)
 *   --folder NAME    the entry's folder under PSP/GAME (default: from the title)
 *   --template PBP   the launcher EBOOT.PBP (default: the one built in) */
#include "xw_core.h"
#include <stdio.h>
#include <string.h>

#ifdef XW_EMBED_TEMPLATE
#include "launcher_pbp.h"     /* xw_launcher_pbp[], xw_launcher_pbp_len */
#endif

static void usage(void)
{
   fprintf(stderr,
      "usage: xmbwrap --rom-file <card>/PSP/GAME/<GBAdhoc>/roms/<game> [options]\n"
      "       xmbwrap --root <card> --rom <game relative to roms/> [options]\n"
      "  --title T  --icon IMG  --background IMG  --no-art\n"
      "  --gbadhoc PSP/GAME/GBAdhoc  --folder NAME  --template EBOOT.PBP\n");
}

static int run(int argc, char **argv);

#ifdef _WIN32
/* Windows hands main() its arguments in the ANSI code page, which mangles
 * any non-ASCII path ("Pokémon").  Take them as UTF-16 and pass UTF-8 on:
 * xw_core opens every file through UTF-8 -> UTF-16. */
#include <windows.h>
#include <stdlib.h>
int wmain(int argc, wchar_t **wargv)
{
   char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
   int i;
   SetConsoleOutputCP(CP_UTF8);
   for (i = 0; argv && i < argc; i++)
   {
      int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
      argv[i] = (char *)malloc(n > 0 ? (size_t)n : 1);
      if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL))
         return 1;
   }
   return argv ? run(argc, argv) : 1;
}
#else
int main(int argc, char **argv)
{
   return run(argc, argv);
}
#endif

static int run(int argc, char **argv)
{
   const char *rom_file = NULL, *rom = NULL, *root = NULL, *title = NULL;
   const char *icon = NULL, *bg = NULL, *gb = NULL, *folder = NULL, *tpl = NULL;
   int no_art = 0, i;
   char r_root[1024], r_gb[512], r_rel[1024], t[256], hero[1200], box[1200];
   char install[1600], made[1100], err[1400];
   xw_buf tb = { 0, 0 };
   xw_entry e;

   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name, var) if (!strcmp(a, name)) { if (!v) { usage(); return 2; } var = v; i++; continue; }
      OPT("--rom-file", rom_file)
      OPT("--rom", rom)
      OPT("--root", root)
      OPT("--title", title)
      OPT("--icon", icon)
      OPT("--background", bg)
      OPT("--gbadhoc", gb)
      OPT("--folder", folder)
      OPT("--template", tpl)
#undef OPT
      if (!strcmp(a, "--no-art")) { no_art = 1; continue; }
      usage();
      return 2;
   }
   if (rom_file)
   {
      if (xw_infer(rom_file, r_root, sizeof(r_root), r_gb, sizeof(r_gb), r_rel,
                   sizeof(r_rel)) != 0)
      {
         fprintf(stderr, "xmbwrap: %s is not inside a GBAdhoc install's roms folder\n",
                 rom_file);
         return 1;
      }
      if (!root) root = r_root;
      if (!gb) gb = r_gb;
      if (!rom) rom = r_rel;
   }
   if (!rom || !root)
   {
      usage();
      return 2;
   }
   if (!gb)
      gb = "PSP/GAME/GBAdhoc";
   if (!title)
   {
      xw_title_from_rom(rom, t, sizeof(t));
      title = t;
   }
   if (!no_art)
   {
      snprintf(install, sizeof(install), "%s%s%s", root,
               root[strlen(root) - 1] == '/' || root[strlen(root) - 1] == '\\' ? "" : "/", gb);
      xw_default_art(install, rom, hero, sizeof(hero), box, sizeof(box));
      if (!icon && box[0]) icon = box;
      if (!bg && hero[0]) bg = hero;
   }
   else
      icon = bg = NULL;

   memset(&e, 0, sizeof(e));
   if (tpl)
   {
      if (xw_read_file(tpl, &tb) != 0)
      {
         fprintf(stderr, "xmbwrap: cannot read %s\n", tpl);
         return 1;
      }
      e.template_pbp = tb.data;
      e.template_len = tb.len;
   }
   else
   {
#ifdef XW_EMBED_TEMPLATE
      e.template_pbp = xw_launcher_pbp;
      e.template_len = xw_launcher_pbp_len;
#else
      fprintf(stderr, "xmbwrap: --template is needed (this build has none built in)\n");
      return 2;
#endif
   }
   e.title = title;
   e.rom = rom;
   if (rom_file && rom == r_rel)
   {
      static char psp_rel[1024];
      if (xw_rom_for_psp(rom_file, r_rel, psp_rel, sizeof(psp_rel)) != 0)
         fprintf(stderr, "xmbwrap: warning: \"%s\" has non-ASCII characters and no "
                         "8.3 name was found; the PSP may not open it. Rename it.\n", r_rel);
      e.rom = psp_rel;
   }
   e.gbadhoc = gb;
   e.icon_image = icon;
   e.bg_image = bg;
   e.root = root;
   e.folder = folder;
   if (xw_make_entry(&e, made, sizeof(made), err, sizeof(err)) != 0)
   {
      fprintf(stderr, "xmbwrap: %s\n", err);
      xw_buf_free(&tb);
      return 1;
   }
   printf("made %s\n  title: %s\n  rom: %s\n  gbadhoc: %s\n  icon: %s\n  background: %s\n",
          made, title, rom, gb, icon ? icon : "(none)", bg ? bg : "(none)");
   xw_buf_free(&tb);
   return 0;
}
