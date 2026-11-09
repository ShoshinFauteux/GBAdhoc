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
 * Trading is the one operation users cannot afford to have corrupted.
 *
 * r3-B2 (PLAN HW-2): the copy used to truncate the .bak first and ignore
 * every write and the close, so a full or failing stick left a short .bak in
 * place of the good one; and it read the .sav while the ADR-0025 writer
 * could still be writing it.  Now: the .sav is landed first (flush + sync:
 * the bring-up runs on the emulation thread, so nothing dirties it after),
 * the copy goes to <save>.sav.bak.tmp with every read, write and the close
 * checked, and only a complete copy replaces the .bak.  A failed copy
 * leaves the old .bak as it was; a failed rename leaves the complete copy
 * as .bak.tmp (never neither). */
static void backup_save(const char *save_path)
{
   /* In pieces: a whole-save static buffer was 128 KiB of image (every byte of
    * it off the PSP-1000's ROM cache, docs/MEMORY-BUDGET.md) used only here. */
   uint8_t buf[4096];
   char bak[PSP_BACKUP_PATH_CAP], tmp[PSP_BACKUP_PATH_CAP + 4];
   FILE *in, *out;
   size_t n, total = 0;
   int path_len, bad = 0;

   (void)fe_host_sram_flush(0);   /* the .sav as SRAM stands now ... */
   fe_host_sram_sync();           /* ... and no write of it in flight */
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
   if (path_len < 0 || (size_t)path_len >= sizeof(bak) ||
       snprintf(tmp, sizeof(tmp), "%s.tmp", bak) >= (int)sizeof(tmp))
   {
      fclose(in);
      fe_evt("sav_backup skip=path_too_long");
      return;
   }
   out = fopen(tmp, "wb");
   if (!out)
   {
      fclose(in);
      fe_evt("sav_backup FAILED at=open");
      return;
   }
   while (n)
   {
      if (fwrite(buf, 1, n, out) != n)
      {
         bad = 1;
         break;
      }
      total += n;
      n = total < FE_SRAM_SIZE ? FE_SRAM_SIZE - total : 0;
      if (n > sizeof(buf))
         n = sizeof(buf);
      if (n)
      {
         n = fread(buf, 1, n, in);
         if (!n && ferror(in))
            bad = 2;
      }
   }
   fclose(in);
   if (fclose(out) != 0 && !bad)
      bad = 3;
   if (!bad)
   {
      remove(bak);
      (void)rename(tmp, bak);
      /* PSP and PPSSPP can disagree about rename's return value (fe_host's
       * state_file_rename): check the names instead. */
      if (!file_exists(bak) || file_exists(tmp))
         bad = 4;
   }
   if (bad)
   {
      if (bad != 4)
         remove(tmp);
      fe_evt("sav_backup FAILED at=%s size=%u", bad == 1 ? "write" :
             bad == 2 ? "read" : bad == 3 ? "close" : "rename",
             (unsigned)total);
      return;
   }
   fe_evt("sav_backup file=%s size=%u", bak, (unsigned)total);
}


