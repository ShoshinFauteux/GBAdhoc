/* launcher/main.c -- a game's own XMB entry (issue #29, docs/XMB-LAUNCH.md).
 *
 * One small EBOOT, the same for every game.  What makes an entry a particular
 * game is the PARAM.SFO title, the ICON0/PIC1 art and the `launch.ini` beside
 * it, all written by the entry maker (tools/xmbwrap).  This program reads
 * launch.ini and starts the installed GBAdhoc with
 *
 *     argv[0] = <GBAdhoc>/EBOOT.PBP   argv[1] = "launch"   argv[2] = <rom>
 *
 * so the game runs in the one install: its saves, states, settings, cheats
 * and art are the game list's own, and updating GBAdhoc updates every entry.
 * No ROM is copied into an entry.
 *
 * launch.ini (key = value, # comments):
 *   rom      the ROM, relative to GBAdhoc's roms/ (or an absolute dev:/ path)
 *   gbadhoc  GBAdhoc's folder, default PSP/GAME/GBAdhoc; a bare folder is
 *            looked for on this entry's own device first (ms0: or ef0:), then
 *            on the other one, so an entry on a PSP Go's internal storage can
 *            start GBAdhoc on its Memory Stick and the other way round.
 *
 * Launching: the firmware has a different launch call per device, and on CFW
 * an EBOOT must be started the way the firmware starts it (GBAdhoc's own
 * relaunch_eboot does the same): Ms2 for the Memory Stick, Ef2 for ef0:. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <systemctrl.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("GBAdhocLauncher", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(256);

#define PATH_CAP 320

static int file_exists(const char *path)
{
   SceIoStat st;
   return sceIoGetstat(path, &st) >= 0 && !FIO_S_ISDIR(st.st_mode);
}

/* `key = value` from an ini held in memory; trims spaces and a trailing \r. */
static int ini_get(const char *buf, const char *key, char *out, int cap)
{
   size_t klen = strlen(key);
   const char *p = buf;
   while (*p)
   {
      const char *line = p, *eol = strchr(p, '\n');
      const char *v, *end;
      p = eol ? eol + 1 : line + strlen(line);
      end = eol ? eol : p;
      while (line < end && (*line == ' ' || *line == '\t'))
         line++;
      if (line >= end || *line == '#' || *line == ';')
         continue;
      if ((size_t)(end - line) <= klen || strncmp(line, key, klen) != 0)
         continue;
      v = line + klen;
      while (v < end && (*v == ' ' || *v == '\t'))
         v++;
      if (v >= end || *v != '=')
         continue;
      v++;
      while (v < end && (*v == ' ' || *v == '\t'))
         v++;
      while (end > v && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
         end--;
      if (end - v >= cap)
         return -1;
      memcpy(out, v, (size_t)(end - v));
      out[end - v] = '\0';
      return 0;
   }
   return -1;
}

/* Something went wrong: say what, in words, then back to the XMB. */
static void fail(const char *what, const char *detail)
{
   SceCtrlData pad;
   int i;
   pspDebugScreenInit();
   pspDebugScreenSetXY(2, 4);
   pspDebugScreenPrintf("GBAdhoc launcher\n\n  %s\n\n  %s\n\n"
                        "  Press any button to return to the XMB.", what,
                        detail ? detail : "");
   sceCtrlSetSamplingCycle(0);
   for (i = 0; i < 30 * 60; i++)          /* at most ~30 s */
   {
      sceCtrlPeekBufferPositive(&pad, 1);
      if (i > 30 && pad.Buttons)
         break;
      sceDisplayWaitVblankStart();
   }
   sceKernelExitGame();
}

int main(int argc, char *argv[])
{
   static char ini[2048], dir[PATH_CAP], dev[8], rom[PATH_CAP], gb[PATH_CAP];
   static char eboot[PATH_CAP], romcheck[PATH_CAP * 2], args[PATH_CAP * 3];
   struct SceKernelLoadExecVSHParam param;
   const char *self = (argc > 0 && argv && argv[0]) ? argv[0] : "ms0:/";
   const char *slash = strrchr(self, '/'), *colon = strchr(self, ':');
   char path[PATH_CAP + 16];
   int fd, n, rc;
   size_t a;

   /* This entry's folder and device ("ms0:" / "ef0:"). */
   snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - self) : 0, self);
   snprintf(dev, sizeof(dev), "%.*s", colon ? (int)(colon - self + 1) : 0, self);
   if (!dev[0])
      snprintf(dev, sizeof(dev), "ms0:");

   snprintf(path, sizeof(path), "%s/launch.ini", dir);
   fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   if (fd < 0)
      fail("This entry has no launch.ini.", path);
   n = sceIoRead(fd, ini, sizeof(ini) - 1);
   sceIoClose(fd);
   ini[n > 0 ? n : 0] = '\0';
   if (ini_get(ini, "rom", rom, sizeof(rom)) != 0 || !rom[0])
      fail("launch.ini does not name a ROM.", path);
   if (ini_get(ini, "gbadhoc", gb, sizeof(gb)) != 0 || !gb[0])
      snprintf(gb, sizeof(gb), "PSP/GAME/GBAdhoc");

   /* Where GBAdhoc is: as given, or on this device, then the other one. */
   eboot[0] = '\0';
   if (strchr(gb, ':'))
   {
      size_t l = strlen(gb);
      if (l > 4 && strcasecmp(gb + l - 4, ".PBP") == 0)
         snprintf(eboot, sizeof(eboot), "%s", gb);
      else
         snprintf(eboot, sizeof(eboot), "%s/EBOOT.PBP", gb);
      if (!file_exists(eboot))
         fail("GBAdhoc is not where launch.ini says.", eboot);
   }
   else
   {
      const char *other = strcasecmp(dev, "ef0:") == 0 ? "ms0:" : "ef0:";
      snprintf(eboot, sizeof(eboot), "%s/%s/EBOOT.PBP", dev, gb);
      if (!file_exists(eboot))
      {
         snprintf(eboot, sizeof(eboot), "%s/%s/EBOOT.PBP", other, gb);
         if (!file_exists(eboot))
         {
            snprintf(path, sizeof(path), "%s/%s", dev, gb);
            fail("GBAdhoc was not found. Install it, or fix `gbadhoc` in launch.ini.",
                 path);
         }
      }
   }

   /* The ROM, checked here so a missing one is named on screen. */
   if (strchr(rom, ':'))
      snprintf(romcheck, sizeof(romcheck), "%s", rom);
   else
   {
      const char *s = strrchr(eboot, '/');
      snprintf(romcheck, sizeof(romcheck), "%.*s/roms/%s",
               s ? (int)(s - eboot) : 0, eboot, rom);
   }
   if (!file_exists(romcheck))
      fail("The game's ROM is not in GBAdhoc's roms folder.", romcheck);

   /* argv for GBAdhoc: EBOOT path, "launch", ROM. */
   a = 0;
   n = snprintf(args + a, sizeof(args) - a, "%s", eboot);    a += (size_t)n + 1;
   n = snprintf(args + a, sizeof(args) - a, "launch");       a += (size_t)n + 1;
   n = snprintf(args + a, sizeof(args) - a, "%s", rom);      a += (size_t)n + 1;
   if (a > sizeof(args))
      fail("The paths in launch.ini are too long.", rom);

   memset(&param, 0, sizeof(param));
   param.size = sizeof(param);
   param.args = (SceSize)a;
   param.argp = args;
   param.key  = "game";
   if (strncasecmp(eboot, "ef0:", 4) == 0)
      rc = sctrlKernelLoadExecVSHEf2(eboot, &param);
   else
      rc = sctrlKernelLoadExecVSHMs2(eboot, &param);

   /* Only returns on failure. */
   snprintf(path, sizeof(path), "%s (error %08x)", eboot, (unsigned)rc);
   fail("The PSP refused to start GBAdhoc.", path);
   return 0;
}
