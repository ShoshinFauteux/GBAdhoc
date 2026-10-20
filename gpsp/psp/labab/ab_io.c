/* ab_io.c -- see ab_io.h.  Only sceIo / sceRtc; no libc. */
#include <pspiofilemgr.h>
#include <psprtc.h>
#include "ab_io.h"

int ab_exists(const char *path)
{
   SceIoStat st;
   return sceIoGetstat(path, &st) >= 0;
}

int ab_read_file(const char *path, char *buf, int cap)
{
   int fd, n, total = 0;
   char extra;
   fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   if (fd < 0) return -1;
   while (total < cap)
   {
      n = sceIoRead(fd, buf + total, cap - total);
      if (n < 0) { sceIoClose(fd); return -1; }
      if (n == 0) break;
      total += n;
   }
   if (total == cap && sceIoRead(fd, &extra, 1) > 0) { sceIoClose(fd); return -1; }
   sceIoClose(fd);
   return total;
}

static char g_verify[AB_FILE_MAX + 1];

int ab_write_verify(const char *path, const char *buf, int len)
{
   int fd, n, i;
   if (len < 0 || len > AB_FILE_MAX) return -1;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0) return -1;
   n = sceIoWrite(fd, buf, len);
   if (sceIoClose(fd) < 0 || n != len) return -1;
   n = ab_read_file(path, g_verify, AB_FILE_MAX);
   if (n != len) return -1;
   for (i = 0; i < len; i++)
      if (g_verify[i] != buf[i]) return -1;
   return 0;
}

int ab_is_pbp(const char *path)
{
   unsigned char m[4];
   int fd = sceIoOpen(path, PSP_O_RDONLY, 0), n;
   if (fd < 0) return 0;
   n = sceIoRead(fd, m, 4);
   sceIoClose(fd);
   return n == 4 && m[0] == 0 && m[1] == 'P' && m[2] == 'B' && m[3] == 'P';
}

void ab_log_line(const char *path, const char *old_path, const char *line)
{
   SceIoStat st;
   int fd, mode = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND;
   if (sceIoGetstat(path, &st) >= 0 && st.st_size > AB_LOG_MAX)
   {
      sceIoRemove(old_path);
      sceIoRename(path, old_path);
      /* If the rename did not take, start over rather than grow without bound. */
      if (sceIoGetstat(path, &st) >= 0 && st.st_size > AB_LOG_MAX)
         mode = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
   }
   fd = sceIoOpen(path, mode, 0777);
   if (fd < 0) return;
   sceIoWrite(fd, line, ab_strlen(line));
   sceIoWrite(fd, "\n", 1);
   sceIoClose(fd);
}

ab_u64 ab_rtc_now(void)
{
   u64 tick = 0;
   if (sceRtcGetCurrentTick(&tick) < 0 || tick == 0) return 0;
   return (ab_u64)(tick / 1000000ull);
}

static char g_text[AB_FILE_MAX + 1];

int ab_budget_load(const char *path, ab_budget *b)
{
   int n = ab_read_file(path, g_text, AB_FILE_MAX);
   if (n < 0) return AB_E_MISSING;
   return ab_budget_parse(g_text, n, b);
}

int ab_budget_store(const char *path, const ab_budget *b)
{
   int n = ab_budget_format(b, g_text, AB_FILE_MAX);
   if (n <= 0) return -1;
   return ab_write_verify(path, g_text, n);
}
