/* Bounds and checked builders for paths derived from browser ROM names.
 * A browser entry can contain up to 319 relative-path bytes; with the PSP
 * install path and sidecar suffixes, 512 bytes leaves room for all products. */
#ifndef PSP_ROM_PATHS_H
#define PSP_ROM_PATHS_H

#include <stdio.h>
#include <string.h>

#define PSP_ROM_REL_PATH_CAP 320
#define PSP_FILE_PATH_CAP    512
#define PSP_BACKUP_PATH_CAP  (PSP_FILE_PATH_CAP + 4) /* ".bak" + NUL */

static inline int psp_rom_path_join(char *out, size_t out_sz,
                                    const char *root, const char *relative)
{
   int n;
   if (!out || !out_sz || !root || !relative)
      return -1;
   n = snprintf(out, out_sz, "%s/%s", root, relative);
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* Replace the final extension with suffix. Source and destination may alias. */
static inline int psp_rom_path_suffix(char *out, size_t out_sz,
                                      const char *path, const char *suffix)
{
   size_t n, suffix_len;
   const char *dot;
   if (!out || !out_sz || !path || !suffix)
      return -1;
   n = strlen(path);
   dot = strrchr(path, '.');
   if (dot)
      n = (size_t)(dot - path);
   suffix_len = strlen(suffix);
   if (n + suffix_len + 1 > out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   if (out != path)
      memmove(out, path, n);
   memcpy(out + n, suffix, suffix_len + 1);
   return 0;
}

#endif
