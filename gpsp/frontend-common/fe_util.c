#include "fe_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ crc32 */

static uint32_t crc_table[256];
static int      crc_table_ready = 0;

static void crc32_init(void)
{
   uint32_t i, j;
   for (i = 0; i < 256; i++)
   {
      uint32_t c = i;
      for (j = 0; j < 8; j++)
         c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      crc_table[i] = c;
   }
   crc_table_ready = 1;
}

uint32_t fe_crc32(uint32_t crc, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   if (!crc_table_ready)
      crc32_init();
   crc = ~crc;
   while (len--)
      crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
   return ~crc;
}

/* ------------------------------------------------------------------- sha1 */

typedef struct
{
   uint32_t h[5];
   uint64_t len;
   uint8_t  buf[64];
   size_t   buf_used;
} sha1_ctx;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
   uint32_t w[80];
   uint32_t a, b, cc, d, e;
   int i;

   for (i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
             ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
   for (i = 16; i < 80; i++)
      w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

   a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
   for (i = 0; i < 80; i++)
   {
      uint32_t f, k;
      if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5A827999; }
      else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
      else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6; }
      {
         uint32_t t = rol32(a, 5) + f + e + k + w[i];
         e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
      }
   }
   c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void fe_sha1_hex(const void *data, size_t len, char *out_hex)
{
   sha1_ctx c;
   const uint8_t *p = (const uint8_t *)data;
   size_t n = len;
   uint8_t pad[128];
   size_t pad_len;
   int i;

   c.h[0] = 0x67452301; c.h[1] = 0xEFCDAB89; c.h[2] = 0x98BADCFE;
   c.h[3] = 0x10325476; c.h[4] = 0xC3D2E1F0;
   c.len = (uint64_t)len * 8;
   c.buf_used = 0;

   while (n >= 64)
   {
      sha1_block(&c, p);
      p += 64;
      n -= 64;
   }

   memset(pad, 0, sizeof(pad));
   memcpy(pad, p, n);
   pad[n] = 0x80;
   pad_len = (n < 56) ? 64 : 128;
   for (i = 0; i < 8; i++)
      pad[pad_len - 1 - i] = (uint8_t)(c.len >> (8 * i));
   sha1_block(&c, pad);
   if (pad_len == 128)
      sha1_block(&c, pad + 64);

   for (i = 0; i < 5; i++)
      sprintf(out_hex + i * 8, "%08x", (unsigned)c.h[i]);
   out_hex[40] = '\0';
}

/* The streaming form reuses sha1_block; fe_sha1_hex above is untouched. */
void fe_sha1_init(fe_sha1_ctx *c)
{
   c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
   c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
   c->len = 0;
   c->used = 0;
}

void fe_sha1_update(fe_sha1_ctx *c, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   sha1_ctx b;
   memcpy(b.h, c->h, sizeof(b.h));
   c->len += (uint64_t)len * 8;
   while (len)
   {
      size_t n = 64 - c->used;
      if (n > len)
         n = len;
      memcpy(c->buf + c->used, p, n);
      c->used += n;
      p += n;
      len -= n;
      if (c->used == 64)
      {
         sha1_block(&b, c->buf);
         c->used = 0;
      }
   }
   memcpy(c->h, b.h, sizeof(c->h));
}

void fe_sha1_final(fe_sha1_ctx *c, uint8_t out[20])
{
   uint8_t pad[128];
   size_t pad_len;
   sha1_ctx b;
   int i;
   memcpy(b.h, c->h, sizeof(b.h));
   memset(pad, 0, sizeof(pad));
   memcpy(pad, c->buf, c->used);
   pad[c->used] = 0x80;
   pad_len = (c->used < 56) ? 64 : 128;
   for (i = 0; i < 8; i++)
      pad[pad_len - 1 - i] = (uint8_t)(c->len >> (8 * i));
   sha1_block(&b, pad);
   if (pad_len == 128)
      sha1_block(&b, pad + 64);
   for (i = 0; i < 5; i++)
   {
      out[i * 4] = (uint8_t)(b.h[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(b.h[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(b.h[i] >> 8);
      out[i * 4 + 3] = (uint8_t)b.h[i];
   }
}

/* -------------------------------------------------------------------- bmp */

static void put16(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t *p, uint32_t v)
{
   p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
   p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int fe_bmp_write_rgb565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch)
{
   FILE *f;
   uint8_t hdr[54];
   unsigned row_bytes = w * 3;
   unsigned row_pad   = (4 - (row_bytes & 3)) & 3;
   uint32_t img_size  = (row_bytes + row_pad) * h;
   unsigned x, y;
   uint8_t *row;

   f = fopen(path, "wb");
   if (!f)
      return -1;
   row = (uint8_t *)malloc(row_bytes + row_pad);
   if (!row)
   {
      fclose(f);
      return -1;
   }

   memset(hdr, 0, sizeof(hdr));
   hdr[0] = 'B'; hdr[1] = 'M';
   put32(hdr + 2, 54 + img_size);
   put32(hdr + 10, 54);
   put32(hdr + 14, 40);
   put32(hdr + 18, w);
   put32(hdr + 22, h);
   put16(hdr + 26, 1);
   put16(hdr + 28, 24);
   put32(hdr + 34, img_size);
   fwrite(hdr, 1, 54, f);

   memset(row + row_bytes, 0, row_pad);
   for (y = 0; y < h; y++)
   {
      /* BMP rows are bottom-up */
      const uint16_t *src =
         (const uint16_t *)((const uint8_t *)pix + (size_t)(h - 1 - y) * pitch);
      for (x = 0; x < w; x++)
      {
         uint16_t c = src[x];
         uint8_t r = (uint8_t)((c >> 11) & 0x1F);
         uint8_t g = (uint8_t)((c >> 5) & 0x3F);
         uint8_t b = (uint8_t)(c & 0x1F);
         row[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
         row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
         row[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
      }
      fwrite(row, 1, row_bytes + row_pad, f);
   }

   free(row);
   fclose(f);
   return 0;
}

int fe_bmp_write_psp565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch)
{
   FILE *f;
   uint8_t hdr[54];
   unsigned row_bytes = w * 3;
   unsigned row_pad   = (4 - (row_bytes & 3)) & 3;
   uint32_t img_size  = (row_bytes + row_pad) * h;
   unsigned x, y;
   uint8_t *row;

   f = fopen(path, "wb");
   if (!f)
      return -1;
   row = (uint8_t *)malloc(row_bytes + row_pad);
   if (!row)
   {
      fclose(f);
      return -1;
   }

   memset(hdr, 0, sizeof(hdr));
   hdr[0] = 'B'; hdr[1] = 'M';
   put32(hdr + 2, 54 + img_size);
   put32(hdr + 10, 54);
   put32(hdr + 14, 40);
   put32(hdr + 18, w);
   put32(hdr + 22, h);
   put16(hdr + 26, 1);
   put16(hdr + 28, 24);
   put32(hdr + 34, img_size);
   fwrite(hdr, 1, 54, f);

   memset(row + row_bytes, 0, row_pad);
   for (y = 0; y < h; y++)
   {
      /* BMP rows are bottom-up */
      const uint16_t *src =
         (const uint16_t *)((const uint8_t *)pix + (size_t)(h - 1 - y) * pitch);
      for (x = 0; x < w; x++)
      {
         /* PSP-native 5650 (GE/display format): R bits 0-4, G 5-10, B 11-15
          * — the REVERSE of libretro RGB565. Decoding with this layout is
          * the load-bearing part of the GU color-order regression check. */
         uint16_t c = src[x];
         uint8_t r = (uint8_t)(c & 0x1F);
         uint8_t g = (uint8_t)((c >> 5) & 0x3F);
         uint8_t b = (uint8_t)((c >> 11) & 0x1F);
         row[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
         row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
         row[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
      }
      fwrite(row, 1, row_bytes + row_pad, f);
   }

   free(row);
   fclose(f);
   return 0;
}

/* -------------------------------------------------------------------- ini */

static char *trim(char *s)
{
   char *end;
   while (*s == ' ' || *s == '\t')
      s++;
   end = s + strlen(s);
   while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                      end[-1] == '\r' || end[-1] == '\n'))
      *--end = '\0';
   return s;
}

/* ---- lookup audit (rig: "a misspelled key fails silently") -------------
 *
 * Every harness campaign that lost runs to a config mistake lost them the same
 * way: a key that no code ever reads (a typo, a key from another branch) looks
 * exactly like a key that was applied.  With an audited path registered, every
 * lookup of that file is recorded, and fe_ini_audit_report() walks the file
 * afterwards and reports each key it contains, with whether anything asked for
 * it and what an integer read of it returned.  A key nobody asked for is the
 * typo.  Up to FE_INI_AUDIT_PATHS files, FE_INI_AUDIT_KEYS lookups each.
 *
 * NOT IN A PLAYER BUILD.  Nothing in a release registers a path (the caller
 * is telemetry-only), so the table would be ~46 KB of .bss that shrinks the
 * PSP-1000's heap for nothing.  Same condition as fe_evt.h's
 * GPSP_NO_TELEMETRY. */
#if defined(GPSP_PLAYABLE) && !defined(GPSP_KEEP_TELEMETRY)
void fe_ini_audit(const char *path) { (void)path; }
int fe_ini_audit_report(const char *path,
                        void (*emit)(void *user, const char *key,
                                     const char *raw, int asked, long ival,
                                     int is_int),
                        void *user)
{
   (void)path; (void)emit; (void)user;
   return -1;
}
#define ini_audit_note(path, key) do { (void)(path); (void)(key); } while (0)
#else
#define FE_INI_AUDIT_PATHS 2
/* 384: the 3.5.0 merge (GBA link + in-process return + Save States) took
 * main() to ~207 distinct harness keys, and every lookup past the old 160th
 * went unrecorded -- reported as cfg_unknown, which the rig scorers' I4 rule
 * reads as a misspelled key (INVALID).  A full table now says so itself
 * (__audit_table_full__ below) instead of blaming real keys. */
#define FE_INI_AUDIT_KEYS  384
#define FE_INI_AUDIT_KLEN  40
static struct
{
   char path[256];
   unsigned n;
   char key[FE_INI_AUDIT_KEYS][FE_INI_AUDIT_KLEN];
   unsigned dropped;           /* lookups the full table could not record */
} ini_audit[FE_INI_AUDIT_PATHS];

void fe_ini_audit(const char *path)
{
   unsigned i;
   for (i = 0; i < FE_INI_AUDIT_PATHS; i++)
      if (!ini_audit[i].path[0] || !strcmp(ini_audit[i].path, path))
      {
         strncpy(ini_audit[i].path, path, sizeof(ini_audit[i].path) - 1);
         ini_audit[i].n = 0;
         ini_audit[i].dropped = 0;
         return;
      }
}

static void ini_audit_note(const char *path, const char *key)
{
   unsigned i, k;
   for (i = 0; i < FE_INI_AUDIT_PATHS; i++)
   {
      if (!ini_audit[i].path[0] || strcmp(ini_audit[i].path, path))
         continue;
      for (k = 0; k < ini_audit[i].n; k++)
         if (!strcmp(ini_audit[i].key[k], key))
            return;
      if (ini_audit[i].n >= FE_INI_AUDIT_KEYS)
         ini_audit[i].dropped++;
      if (ini_audit[i].n < FE_INI_AUDIT_KEYS)
      {
         strncpy(ini_audit[i].key[ini_audit[i].n], key, FE_INI_AUDIT_KLEN - 1);
         ini_audit[i].key[ini_audit[i].n][FE_INI_AUDIT_KLEN - 1] = '\0';
         ini_audit[i].n++;
      }
      return;
   }
}

static int ini_audit_asked(unsigned slot, const char *key)
{
   unsigned k;
   for (k = 0; k < ini_audit[slot].n; k++)
      if (!strcmp(ini_audit[slot].key[k], key))
         return 1;
   return 0;
}

static char *trim(char *s);

int fe_ini_audit_report(const char *path,
                        void (*emit)(void *user, const char *key,
                                     const char *raw, int asked, long ival,
                                     int is_int),
                        void *user)
{
   unsigned slot;
   FILE *f;
   char line[512];
   static char seen[FE_INI_AUDIT_KEYS][FE_INI_AUDIT_KLEN];   /* not stack: PSP */
   unsigned nseen = 0, s;
   int n = 0;

   for (slot = 0; slot < FE_INI_AUDIT_PATHS; slot++)
      if (!strcmp(ini_audit[slot].path, path))
         break;
   if (slot == FE_INI_AUDIT_PATHS)
      return -1;
   f = fopen(path, "r");
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
   {
      char *eq, *k, *v, *end;
      long iv;
      if (line[0] == '#' || line[0] == ';' || line[0] == '[')
         continue;
      eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';
      k = trim(line);
      v = trim(eq + 1);
      if (!*k)
         continue;
      iv = strtol(v, &end, 0);
      /* A key that appears twice: fe_ini_get returns the FIRST line, so the
       * later one is silently ignored -- report it as asked=2.  (A rig arm
       * appended `net_latency_ms = 50` below a staged `net_latency_ms = 0`
       * and the run measured nothing; the audit echoed both lines.) */
      for (s = 0; s < nseen; s++)
         if (!strcmp(seen[s], k))
            break;
      if (s < nseen)
      {
         emit(user, k, v, 2, iv, end != v && *trim(end) == '\0');
         n++;
         continue;
      }
      if (nseen < FE_INI_AUDIT_KEYS)
      {
         strncpy(seen[nseen], k, FE_INI_AUDIT_KLEN - 1);
         seen[nseen][FE_INI_AUDIT_KLEN - 1] = '\0';
         nseen++;
      }
      emit(user, k, v, ini_audit_asked(slot, k), iv,
           end != v && *trim(end) == '\0');
      n++;
   }
   fclose(f);
   if (ini_audit[slot].dropped)
   {
      /* unknown on purpose: a scorer that rejects unknown keys rejects the
       * run for the real reason, the table, not for whichever key it lost */
      char cnt[16];
      snprintf(cnt, sizeof(cnt), "%u", ini_audit[slot].dropped);
      emit(user, "__audit_table_full__", cnt, 0, (long)ini_audit[slot].dropped,
           1);
      n++;
   }
   return n;
}
#endif   /* audit only in telemetry builds */

static unsigned ini_lookups, ini_misses;
static const char *ini_never;

void fe_ini_never(const char *path)
{
   ini_never = path;
}

void fe_ini_stats(unsigned *lookups, unsigned *misses)
{
   if (lookups) *lookups = ini_lookups;
   if (misses)  *misses  = ini_misses;
}


/* ---- the in-memory INI document (fe_ini_doc, see fe_util.h) --------------
 *
 * The same reader and writer as fe_ini_get / fe_ini_set, byte for byte, but
 * over a buffer: the file is read once and written once.
 *
 *   READ.  fe_ini_get reads the file with fgets into a 512-byte line, so a
 *   line longer than 511 bytes is seen as several; doc_get walks the buffer
 *   in exactly those pieces and parses each with the same code, so a lookup
 *   answers what the file lookup answered for every input, not just sane
 *   ones.
 *   WRITE.  fe_ini_set's rule: the FIRST line whose text, after leading
 *   blanks, is the key, then blanks, then '=', becomes "key = value\n";
 *   otherwise the line is appended (after a '\n' if the file did not end in
 *   one).  Everything else -- comments, blank lines, unknown keys, order,
 *   CR LF endings on untouched lines, later duplicates -- is kept as it is.
 *   A set that would not fit refuses (returns -1, the document unchanged):
 *   nothing is ever dropped to make room. */

static unsigned ini_doc_hits;        /* lookups served from a document */
static fe_ini_doc *ini_bound;        /* fe_ini_get/set of ini_bound_path   */
static const char *ini_bound_path;   /* use this document                  */

/* fe_ini_get's parse of one fgets() line, shared by file and document. */
static int ini_line_match(char *line, const char *key, char *out,
                          size_t out_sz)
{
   char *eq, *k, *v;
   if (line[0] == '#' || line[0] == ';' || line[0] == '[')
      return 0;
   eq = strchr(line, '=');
   if (!eq)
      return 0;
   *eq = '\0';
   k = trim(line);
   v = trim(eq + 1);
   if (strcmp(k, key) != 0)
      return 0;
   strncpy(out, v, out_sz - 1);
   out[out_sz - 1] = '\0';
   return 1;
}

/* Last resort for a save cut short by a power loss (fe_ini_doc_save): runs
 * only when `path` itself is missing. */
static int ini_exists(const char *p)
{
   FILE *f = fopen(p, "rb");
   if (!f)
      return 0;
   fclose(f);
   return 1;
}

int fe_ini_recover(const char *path)
{
   char tmp[FE_INI_PATH_CAP + 8], bak[FE_INI_PATH_CAP + 8];
   if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
       snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
      return -1;
   if (ini_exists(bak))
   {
      /* The old file was moved aside, so the new one had already been
       * written and closed: it is complete.  Prefer it; else the old. */
      if (ini_exists(tmp) && rename(tmp, path) == 0)
      {
         remove(bak);
         return 1;
      }
      return rename(bak, path) == 0 ? 2 : -1;
   }
   /* No old file moved aside: a .tmp here is a FIRST save that never
    * finished, so it may be a fragment.  Never use it. */
   if (ini_exists(tmp))
      remove(tmp);
   return 0;
}

int fe_ini_doc_load(fe_ini_doc *d, const char *path)
{
   FILE *f;
   size_t n;
   memset(d, 0, sizeof(*d));
   ini_lookups++;                       /* it is a file open, like a get */
   f = fopen(path, "rb");
   if (!f && fe_ini_recover(path) > 0)
      f = fopen(path, "rb");
   if (!f)
   {
      ini_misses++;
      d->buf[0] = '\0';
      d->ok = 1;                         /* absent: an empty document */
      return 0;
   }
   d->existed = 1;
   n = fread(d->buf, 1, sizeof(d->buf) - 1, f);
   if ((n == sizeof(d->buf) - 1 && fgetc(f) != EOF) || ferror(f))
   {
      fclose(f);
      d->buf[0] = '\0';
      return -1;                         /* too big, or unreadable */
   }
   fclose(f);
   d->buf[n] = '\0';
   /* fe_ini_set worked on the C string: a NUL ended the file for it. */
   d->len = strlen(d->buf);
   d->ok = 1;
   return 0;
}

int fe_ini_doc_get(const fe_ini_doc *d, const char *key, char *out,
                   size_t out_sz)
{
   const char *p;
   char line[512];
   if (!d || !d->ok)
      return 0;
   ini_doc_hits++;
   p = d->buf;
   while (*p)
   {
      /* One fgets(line, 512): up to 511 bytes, or through the newline. */
      size_t n = 0;
      while (p[n] && n < sizeof(line) - 1)
         if (p[n++] == '\n')
            break;
      memcpy(line, p, n);
      line[n] = '\0';
      p += n;
      if (ini_line_match(line, key, out, out_sz))
         return 1;
   }
   return 0;
}

long fe_ini_doc_get_int(const fe_ini_doc *d, const char *key, long def)
{
   char buf[64];
   char *end;
   long v;
   if (!fe_ini_doc_get(d, key, buf, sizeof(buf)))
      return def;
   v = strtol(buf, &end, 0);
   return (end == buf) ? def : v;
}

int fe_ini_doc_set(fe_ini_doc *d, const char *key, const char *value)
{
   char rep[1024];                      /* "key = value\n" */
   size_t klen = strlen(key), rlen, ls = 0, le = 0, cap = sizeof(d->buf) - 1;
   int written, found = 0;
   const char *line;

   if (!d || !d->ok)
      return -1;
   written = snprintf(rep, sizeof(rep), "%s = %s\n", key, value);
   if (written < 0 || (size_t)written >= sizeof(rep))
   {
      d->overflow = 1;
      return -1;
   }
   rlen = (size_t)written;

   for (line = d->buf; *line; )
   {
      const char *nl = strchr(line, '\n');
      size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (strncmp(p, key, klen) == 0)
      {
         const char *q = p + klen;
         while (*q == ' ' || *q == '\t')
            q++;
         if (*q == '=')
         {
            ls = (size_t)(line - d->buf);
            le = ls + len;
            found = 1;
            break;
         }
      }
      line += len;
   }

   if (found)
   {
      if (le - ls == rlen && memcmp(d->buf + ls, rep, rlen) == 0)
         return 0;                       /* already says exactly this */
      if (d->len - (le - ls) + rlen > cap)
      {
         d->overflow = 1;
         return -1;
      }
      memmove(d->buf + ls + rlen, d->buf + le, d->len - le + 1);
      memcpy(d->buf + ls, rep, rlen);
      d->len = d->len - (le - ls) + rlen;
   }
   else
   {
      size_t need = rlen + (d->len && d->buf[d->len - 1] != '\n');
      if (d->len + need > cap)
      {
         d->overflow = 1;
         return -1;
      }
      if (d->len && d->buf[d->len - 1] != '\n')
         d->buf[d->len++] = '\n';
      memcpy(d->buf + d->len, rep, rlen + 1);
      d->len += rlen;
   }
   d->changed = 1;
   return 0;
}

int fe_ini_doc_set_int(fe_ini_doc *d, const char *key, long value)
{
   char buf[32];
   snprintf(buf, sizeof(buf), "%ld", value);
   return fe_ini_doc_set(d, key, buf);
}

static int ini_write_file(const char *p, const char *data, size_t n)
{
   FILE *f = fopen(p, "wb");
   int bad;
   if (!f)
      return -1;
   bad = fwrite(data, 1, n, f) != n;
   bad |= fflush(f) != 0;
   bad |= fclose(f) != 0;
   return bad ? -1 : 0;
}

/* ATOMIC REPLACE, WITHOUT TRUSTING RENAME TO OVERWRITE.  Whether the PSP's
 * FAT driver replaces an existing target on sceIoRename is not something to
 * bet a config on, so it is never asked to:
 *
 *   1. remove path.bak (a leftover)          path = old
 *   2. write path.tmp completely, close it   path = old, tmp = new
 *   3. rename path -> path.bak               bak = old, tmp = new (complete)
 *   4. rename path.tmp -> path               path = new
 *   5. remove path.bak                       path = new
 *
 * A power cut anywhere leaves either `path` (old or new, whole) or, between
 * 3 and 4, no `path` but a complete tmp beside the old bak -- which
 * fe_ini_recover (run by the next load) turns back into `path`, preferring
 * the new.  A tmp with no bak beside it is a first save that never finished
 * and is never used.  If a rename fails, the file is put back and written
 * in place instead (the old, non-atomic way, but once, not per key). */
int fe_ini_doc_save(fe_ini_doc *d, const char *path)
{
   char tmp[FE_INI_PATH_CAP + 8], bak[FE_INI_PATH_CAP + 8];
   int moved = 0;
   if (!d || !d->ok)
      return -1;
   if (!d->changed)
      return d->overflow ? -1 : 0;       /* byte-identical: nothing to do */
   if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
       snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
      return -1;
   remove(bak);
   if (ini_write_file(tmp, d->buf, d->len) != 0)
   {
      remove(tmp);
      return -1;
   }
   if (d->existed)
      moved = rename(path, bak) == 0;
   if (rename(tmp, path) != 0)
   {
      int rc;
      if (moved)
         rename(bak, path);
      rc = ini_write_file(path, d->buf, d->len);
      remove(tmp);
      d->save_fallback = 1;
      return rc ? -1 : (d->overflow ? -1 : 0);
   }
   if (moved)
      remove(bak);
   d->existed = 1;
   d->changed = 0;
   return d->overflow ? -1 : 0;
}

void fe_ini_doc_bind(fe_ini_doc *d, const char *path)
{
#ifdef FE_INI_DOC_LEGACY
   /* tools/tests/test_ini_doc.c: the same callers on the per-key path. */
   d = NULL;
#endif
   ini_bound = d && path ? d : NULL;
   ini_bound_path = ini_bound ? path : NULL;
}

static fe_ini_doc *ini_doc_for(const char *path)
{
   return (ini_bound && ini_bound_path && strcmp(path, ini_bound_path) == 0)
      ? ini_bound : NULL;
}

unsigned fe_ini_doc_hits(void)
{
   return ini_doc_hits;
}

int fe_ini_get(const char *path, const char *key, char *out, size_t out_sz)
{
   FILE *f;
   /* `last_rom` can hold a browser relative path of 319 bytes plus the
    * `key = ` prefix; keep the whole setting on one line when loading it. */
   char line[512];
   int found = 0;

   ini_audit_note(path, key);
   if (ini_never && strcmp(path, ini_never) == 0)
      return 0;
   if (ini_doc_for(path))
      return fe_ini_doc_get(ini_doc_for(path), key, out, out_sz);
   f = fopen(path, "r");
   ini_lookups++;
   if (!f)
   {
      ini_misses++;
      return 0;
   }
   while (fgets(line, sizeof(line), f))
   {
      char *eq, *k, *v;
      if (line[0] == '#' || line[0] == ';' || line[0] == '[')
         continue;
      eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';
      k = trim(line);
      v = trim(eq + 1);
      if (strcmp(k, key) == 0)
      {
         strncpy(out, v, out_sz - 1);
         out[out_sz - 1] = '\0';
         found = 1;
         break;
      }
   }
   fclose(f);
   return found;
}

long fe_ini_get_int(const char *path, const char *key, long def)
{
   char buf[64];
   char *end;
   long v;
   if (!fe_ini_get(path, key, buf, sizeof(buf)))
      return def;
   v = strtol(buf, &end, 0);
   return (end == buf) ? def : v;
}

int fe_ini_set(const char *path, const char *key, const char *value)
{
   /* Small-config writer (settings persistence, plan §8): read the whole
    * file, replace the key's line or append it, rewrite.  Files are a few
    * hundred bytes; 4 KiB is generous. */
   char buf[4096];
   char out[4096 + 320];
   size_t n = 0, o = 0, klen = strlen(key);
   int replaced = 0;
   FILE *f;

   if (ini_doc_for(path))
      return fe_ini_doc_set(ini_doc_for(path), key, value);
   f = fopen(path, "r");
   if (f)
   {
      n = fread(buf, 1, sizeof(buf) - 1, f);
      if (n == sizeof(buf) - 1 && fgetc(f) != EOF)
      {
         fclose(f);
         return -1;
      }
      if (ferror(f))
      {
         fclose(f);
         return -1;
      }
      fclose(f);
   }
   buf[n] = '\0';

   {
      char *line = buf;
      while (line && *line)
      {
         char *nl = strchr(line, '\n');
         size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
         const char *p = line;
         while (*p == ' ' || *p == '\t')
            p++;
         if (!replaced && strncmp(p, key, klen) == 0)
         {
            const char *q = p + klen;
            while (*q == ' ' || *q == '\t')
               q++;
            if (*q == '=')
            {
               int written;
               if (o >= sizeof(out))
                  return -1;
               written = snprintf(out + o, sizeof(out) - o, "%s = %s\n",
                                  key, value);
               if (written < 0 || (size_t)written >= sizeof(out) - o)
                  return -1;
               o += (size_t)written;
               replaced = 1;
               line = nl ? nl + 1 : NULL;
               continue;
            }
         }
         if (len >= sizeof(out) - o)
            return -1;
         memcpy(out + o, line, len);
         o += len;
         line = nl ? nl + 1 : NULL;
      }
   }
   if (!replaced)
   {
      if (o && out[o - 1] != '\n')
      {
         if (o >= sizeof(out))
            return -1;
         out[o++] = '\n';
      }
      if (o >= sizeof(out))
         return -1;
      {
         int written = snprintf(out + o, sizeof(out) - o, "%s = %s\n",
                                key, value);
         if (written < 0 || (size_t)written >= sizeof(out) - o)
            return -1;
         o += (size_t)written;
      }
   }

   f = fopen(path, "wb");
   if (!f)
      return -1;
   if (fwrite(out, 1, o, f) != o)
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   return 0;
}

int fe_ini_set_int(const char *path, const char *key, long value)
{
   char buf[32];
   snprintf(buf, sizeof(buf), "%ld", value);
   return fe_ini_set(path, key, buf);
}
