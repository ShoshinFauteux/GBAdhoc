/* test_fe_sha1.c -- fe_sha1_hex and fe_sha1_init/update/final against known
 * SHA-1 digests.
 *
 * Why: link play compares the two consoles' ROM hashes (fe_gblink, fe_host).
 * Both functions once padded the last block in a 72-byte buffer, but a tail
 * of 56-63 bytes needs two blocks (128 bytes): the length went past the
 * array and the second block hashed whatever was on the stack.  Such carts
 * got a digest that differed between consoles, and link refused them with
 * rom_hash_mismatch.  The lengths below cover each side of both padding
 * boundaries (55/56 and 119/120), the exact block sizes, and 376 (5 blocks
 * plus a 56-byte tail).
 *
 * The expected digests were computed with the system sha1sum over the same
 * bytes: msg[i] = (i * 31 + 7) & 0xFF.  The 56-byte vector is the FIPS 180
 * test message, whose SHA-1 is published.  Before every call the stack is
 * filled with a different byte, so a read of uninitialised stack shows up as
 * a wrong digest even without the sanitizer. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fe_util.h"

static const struct { size_t len; const char *sha1; } vec[] = {
   {   0, "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
   {   1, "5d1be7e9dda1ee8896be5b7e34a85ee16452a7b4" },
   {  55, "749bbefb28edc4638b28b2b9a9e03ab9a4032b90" },
   {  56, "a5b6e9c29d201c774753ff8e7fb64931656f5e63" },
   {  57, "eb0737bed5451790722b2df351829ce117e3d9dd" },
   {  63, "d1a454409359fc372b4d22b3cea6488d6ba1be00" },
   {  64, "39a0d8b645ad85f1f976731ed112ac9455e28b78" },
   { 119, "562ecf8a430f8e1056e3619bae33628e9a1d0a4e" },
   { 120, "353f6d2bf0e91aa91b74a2e0b3f297510f7d825f" },
   { 127, "bebc42d2d3d1e5fb8ad8895c2dcef2d68a6c279a" },
   { 128, "0060f2a7e34b6e4d459f560197ef93243732a400" },
   { 376, "0916e909c901c74b3bd61599ba04f7b107783b67" },
};

static const char nist56[] =
   "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char nist56_sha1[] = "84983e441c3bd26ebaae4aa1f95129e5e54670f1";

static int fails;

/* Fill the stack below the caller with v, so a buffer read past its end
 * sees v instead of whatever an earlier call left there. */
static void __attribute__((noinline)) scribble(unsigned char v)
{
   volatile unsigned char big[4096];
   size_t i;
   for (i = 0; i < sizeof(big); i++)
      big[i] = v;
}

static void check(const char *what, size_t len, const char *got,
                  const char *want)
{
   if (strcmp(got, want))
   {
      printf("FAIL %-14s len=%3zu got %s want %s\n", what, len, got, want);
      fails++;
   }
}

static void to_hex(const uint8_t d[20], char out[41])
{
   int i;
   for (i = 0; i < 20; i++)
      sprintf(out + 2 * i, "%02x", d[i]);
}

/* fe_sha1_update fed `step` bytes at a time (0 = all at once). */
static void streamed(const uint8_t *m, size_t len, size_t step, char out[41])
{
   fe_sha1_ctx c;
   uint8_t d[20];
   size_t off = 0;
   fe_sha1_init(&c);
   if (!step)
      step = len ? len : 1;
   while (off < len)
   {
      size_t n = len - off < step ? len - off : step;
      fe_sha1_update(&c, m + off, n);
      off += n;
   }
   fe_sha1_final(&c, d);
   to_hex(d, out);
}

static void check_all(const char *name, const uint8_t *m, size_t len,
                      const char *want)
{
   static const unsigned char fill[] = { 0x00, 0xA5, 0xFF };
   static const size_t steps[] = { 0, 1, 7, 64 };
   char got[41], what[32];
   size_t f, s;

   /* Unbuffered (main), so a crash names the length that caused it. */
   printf("  %-7s len=%3zu\n", name, len);
   for (f = 0; f < sizeof(fill); f++)
   {
      scribble(fill[f]);
      fe_sha1_hex(m, len, got);
      snprintf(what, sizeof(what), "%s hex", name);
      check(what, len, got, want);

      for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
      {
         scribble(fill[f]);
         streamed(m, len, steps[s], got);
         snprintf(what, sizeof(what), "%s final/%zu", name, steps[s]);
         check(what, len, got, want);
      }
   }
}

int main(void)
{
   static uint8_t msg[512];
   size_t i;

   setvbuf(stdout, NULL, _IONBF, 0);
   for (i = 0; i < sizeof(msg); i++)
      msg[i] = (uint8_t)(i * 31 + 7);

   check_all("nist56", (const uint8_t *)nist56, 56, nist56_sha1);
   for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++)
      check_all("pattern", msg, vec[i].len, vec[i].sha1);

   if (fails)
   {
      printf("test_fe_sha1: %d mismatches\n", fails);
      return 1;
   }
   printf("test_fe_sha1: %zu lengths + NIST 56-byte vector OK "
          "(hex and streaming, 3 stack fills, 4 chunk sizes)\n",
          sizeof(vec) / sizeof(vec[0]));
   return 0;
}
