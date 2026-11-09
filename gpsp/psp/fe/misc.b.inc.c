static void dump_ge_bmp(void)
{
   char path[176];
   snprintf(path, sizeof(path), "%s/ge_%06u.bmp", LOG_DIR,
            fe_host_frame_count());
   if (vid_dump_ge(path) == 0)
      fe_evt("ge_dump file=%s w=%d h=%d", path, VID_SCR_W, VID_SCR_H);
   else
      fe_evt("ge_dump file=%s FAILED", path);
}

static int file_exists(const char *path)
{
   SceIoStat st;
   return sceIoGetstat(path, &st) >= 0;
}

/* Pre-session save backup (plan §4.5): copy <save>.sav -> <save>.sav.bak.
 * Trading is the one operation users cannot afford to have corrupted. */
static void backup_save(const char *save_path)
{
   /* In pieces: a whole-save static buffer was 128 KiB of image (every byte of
    * it off the PSP-1000's ROM cache, docs/MEMORY-BUDGET.md) used only here. */
   uint8_t buf[4096];
   char bak[PSP_BACKUP_PATH_CAP];
   FILE *in, *out;
   size_t n, total = 0;
   int path_len;

   in = fopen(save_path, "rb");
   if (!in)
      return;
   n = fread(buf, 1, sizeof(buf), in);
   if (!n)
   {
      fclose(in);
      return;
   }
   path_len = snprintf(bak, sizeof(bak), "%s.bak", save_path);
   if (path_len < 0 || (size_t)path_len >= sizeof(bak))
   {
      fclose(in);
      fe_evt("sav_backup skip=path_too_long");
      return;
   }
   out = fopen(bak, "wb");
   if (!out)
   {
      fclose(in);
      return;
   }
   while (n)
   {
      fwrite(buf, 1, n, out);
      total += n;
      n = total < FE_SRAM_SIZE ? FE_SRAM_SIZE - total : 0;
      if (n > sizeof(buf))
         n = sizeof(buf);
      if (n)
         n = fread(buf, 1, n, in);
   }
   fclose(in);
   fclose(out);
   fe_evt("sav_backup file=%s size=%u", bak, (unsigned)total);
}

