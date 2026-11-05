/* test_invite.c -- the invite code (netdrv/invite_code.h): the mockup
 * encoder's own vectors, round trips of both forms, the reading rules, and
 * mistyped-code detection -- exhaustive where it is a guarantee, sampled
 * where it is a rate.
 *
 *   tools/relay/build.sh OUT && OUT/test_invite
 */
#include "invite_code.h"

#include <stdio.h>
#include <string.h>

static unsigned long g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint32_t g_rng = 99;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static void rand_ip(uint8_t ip[4])
{
   do
   {
      uint32_t v = rnd();
      ip[0] = (uint8_t)v; ip[1] = (uint8_t)(v >> 8); ip[2] = (uint8_t)(v >> 16); ip[3] = (uint8_t)(v >> 24);
   } while (!inv_ip_ok(ip));
}

static void strip(const char *in, char *out)   /* the symbols only */
{
   for (; *in; in++)
      if (*in != '-')
         *out++ = *in;
   *out = 0;
}

/* every single wrong symbol and every swap of two different symbols of one
 * code must be refused; counts what it tried */
static void typos(const char *code, unsigned long *singles, unsigned long *swaps)
{
   char c[16], t[16];
   uint8_t got[4];
   unsigned gp;
   int i, j, k, n;
   strip(code, c);
   n = (int)strlen(c);
   for (i = 0; i < n; i++)
      for (k = 0; k < 32; k++)
      {
         if (INV_ALPHABET[k] == c[i])
            continue;
         strcpy(t, c);
         t[i] = INV_ALPHABET[k];
         (*singles)++;
         CHECK(inv_decode(t, got, &gp) != INV_OK, "single typo %s -> %s accepted", c, t);
      }
   for (i = 0; i < n; i++)
      for (j = i + 1; j < n; j++)
      {
         if (c[i] == c[j])
            continue;
         strcpy(t, c);
         t[i] = c[j];
         t[j] = c[i];
         (*swaps)++;
         CHECK(inv_decode(t, got, &gp) != INV_OK, "swap %s -> %s accepted", c, t);
      }
}

static double rate_random(int n, unsigned long tries)
{
   char t[16];
   uint8_t got[4];
   unsigned gp;
   unsigned long k, pass = 0;
   int i;
   for (k = 0; k < tries; k++)
   {
      for (i = 0; i < n; i++)
         t[i] = INV_ALPHABET[rnd() % 32];
      t[n] = 0;
      if (inv_decode(t, got, &gp) == INV_OK)
         pass++;
   }
   return 100.0 * pass / tries;
}

/* The first decoder (24e3e2dd): 64-bit accumulator, byte-wise CRCs.  The
 * shipped one is bit-serial and 32-bit (smaller PSP text); both must give the
 * same answer and the same address and port for every input. */
static int ref_decode(const char *s, uint8_t ip[4], unsigned *port)
{
   uint64_t v = 0;
   uint8_t b[6];
   unsigned chk, p = INV_DEFAULT_PORT;
   int n = 0, x;
   for (; *s; s++)
   {
      x = inv_sym(*s);
      if (x == -2)
         continue;
      if (x < 0)
         return INV_E_SYMBOL;
      if (++n > INV_LONG)
         return INV_E_LENGTH;
      v = (v << 5) | (unsigned)x;
   }
   if (n == INV_SHORT)
   {
      chk = (unsigned)(v & 0xFF);
      v >>= 8;
   }
   else if (n == INV_LONG)
   {
      chk = (unsigned)(v & 0x7F);
      p = (unsigned)((v >> 7) & 0xFFFF);
      v >>= 23;
   }
   else
      return INV_E_LENGTH;
   ip[0] = b[0] = (uint8_t)(v >> 24);
   ip[1] = b[1] = (uint8_t)(v >> 16);
   ip[2] = b[2] = (uint8_t)(v >> 8);
   ip[3] = b[3] = (uint8_t)v;
   b[4] = (uint8_t)(p >> 8);
   b[5] = (uint8_t)p;
   if (n == INV_SHORT ? ((inv_crc8(b, 4) ^ INV_TAG) & 0xFF) != chk
                      : ((inv_crc7(b, 6) ^ INV_TAG) & 0x7F) != chk)
      return INV_E_CHECK;
   if (!inv_ip_ok(ip))
      return INV_E_ADDRESS;
   if (!p)
      return INV_E_PORT;
   *port = p;
   return INV_OK;
}

/* random strings of 0..13 symbols, mostly the alphabet, some folds, dashes and junk */
static void same_as_ref(unsigned long tries, unsigned long *ok)
{
   static const char extra[] = "-- oiluOILU*#";
   char t[20];
   uint8_t a[4], b[4];
   unsigned pa, pb;
   unsigned long k;
   int i, n, ra, rb;
   for (k = 0; k < tries; k++)
   {
      n = (int)(rnd() % 14);
      if (k & 1)
         n = rnd() & 1 ? 8 : 11;      /* half at the real lengths */
      for (i = 0; i < n; i++)
         t[i] = rnd() % 16 ? INV_ALPHABET[rnd() % 32] : extra[rnd() % (sizeof(extra) - 1)];
      t[n] = 0;
      if (k % 4 == 3)
      {
         /* a real code, any address (impossible ones too) and port */
         uint8_t ip[4];
         unsigned port = rnd() % 3 ? (rnd() & 0xFFFF) : INV_DEFAULT_PORT;
         rand_ip(ip);
         if (inv_encode(ip, port, (int)(rnd() & 1), t) != INV_OK)
            continue;
      }
      pa = pb = 12345;
      ra = inv_decode(t, a, &pa);
      rb = ref_decode(t, b, &pb);
      CHECK(ra == rb, "decode(%s) = %d, reference %d", t, ra, rb);
      if (ra == INV_OK && rb == INV_OK)
         CHECK(!memcmp(a, b, 4) && pa == pb, "decode(%s): a different address or port", t);
      *ok += ra == INV_OK;
   }
}

int main(void)
{
   uint8_t ip[4], got[4];
   unsigned port, gp, k;
   char code[16], typo[20];
   int r;
   unsigned long n, singles[2] = { 0, 0 }, swaps[2] = { 0, 0 };

   /* 1. vectors made by the mockup's encoder (builds/xmb-relay-mockup/data.js,
    * run under node): the two implementations must agree byte for byte */
   {
      static const struct { uint8_t ip[4]; unsigned port; const char *code; } v[] = {
         { { 203, 0, 113, 42 }, 27420, "SC07-2AGV" },
         { { 203, 0, 113, 42 }, 27500, "SC07-2AKB-DKN" },
         { { 198, 51, 100, 99 }, 27420, "RRSP-8RTQ" },
         { { 192, 168, 1, 20 }, 27422, "R2M0-253B-3RD" },
         { { 10, 0, 0, 2 }, 37440, "1800-00MJ-80X" },
         { { 1, 2, 3, 4 }, 27420, "0410-615S" },
         { { 223, 255, 255, 255 }, 65534, "VZZZ-ZZZZ-ZSE" },
      };
      for (k = 0; k < sizeof(v) / sizeof(v[0]); k++)
      {
         r = inv_encode(v[k].ip, v[k].port, 1, code);
         /* the short form is the mockup's exactly; the long form shares its
          * address and port symbols (the first 9), its check is a real CRC-7 */
         if (strlen(v[k].code) == 9)
            CHECK(r == INV_OK && !strcmp(code, v[k].code), "vector %u: %s, the mockup says %s", k,
                  code, v[k].code);
         else
            CHECK(r == INV_OK && !strncmp(code, v[k].code, 11), "vector %u: %s, the mockup says %s", k,
                  code, v[k].code);
         r = inv_decode(code, got, &gp);
         CHECK(r == INV_OK && !memcmp(got, v[k].ip, 4) && gp == v[k].port, "vector %u decodes", k);
      }
      printf("     %u vectors agree with the mockup's encoder\n", (unsigned)(sizeof(v) / sizeof(v[0])));
   }

   /* 2. round trips, both forms, with and without dashes */
   for (n = 0; n < 300000; n++)
   {
      rand_ip(ip);
      port = n % 3 ? INV_DEFAULT_PORT : 1 + rnd() % 65535;
      r = inv_encode(ip, port, (int)(n & 1), code);
      CHECK(r == INV_OK, "encode %lu", n);
      CHECK(inv_count(code) == (port == INV_DEFAULT_PORT ? 8 : 11), "count %s", code);
      r = inv_decode(code, got, &gp);
      CHECK(r == INV_OK && !memcmp(got, ip, 4) && gp == port, "round trip %s", code);
   }

   /* 3. reading: case, look-alikes, separators; refusals */
   {
      uint8_t a[4] = { 198, 51, 100, 99 };
      char *p;
      inv_encode(a, 27500, 1, code);
      strcpy(typo, code);
      for (p = typo; *p; p++)
         if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
         else if (*p == '0') *p = 'O';
         else if (*p == '1') *p = (p - typo) % 2 ? 'I' : 'l';
      for (p = typo; *p; p++)
         if (*p == 'v') *p = 'U';
      r = inv_decode(typo, got, &gp);
      CHECK(r == INV_OK && !memcmp(got, a, 4) && gp == 27500, "folded %s (from %s)", typo, code);
      snprintf(typo, sizeof(typo), " %.4s %.4s %.3s ", code, code + 5, code + 10);
      CHECK(inv_decode(typo, got, &gp) == INV_OK, "spaces '%s'", typo);
      CHECK(inv_decode("ABC*0000", got, &gp) == INV_E_SYMBOL, "* refused");
      CHECK(inv_decode("ABCDEFG", got, &gp) == INV_E_LENGTH, "7 symbols refused");
      CHECK(inv_decode("ABCDEFGHJ", got, &gp) == INV_E_LENGTH, "9 symbols refused");
      CHECK(inv_decode("ABCDEFGHJKMNP", got, &gp) == INV_E_LENGTH, "12 symbols refused");
      CHECK(inv_count("KQTR") == 4, "a room code counts 4");
      CHECK(inv_count("SC07-2AGV") == 8 && inv_count("SC07-2AKB-DKN") == 11, "invites count 8 / 11");
      a[0] = 0;
      CHECK(inv_encode(a, 27420, 0, code) == INV_E_ADDRESS, "0.x refused");
      a[0] = 239;
      CHECK(inv_encode(a, 27420, 0, code) == INV_E_ADDRESS, "multicast refused");
      a[0] = 198;
      CHECK(inv_encode(a, 0, 0, code) == INV_E_PORT, "port 0 refused");
   }

   /* 4. mistyped codes: the guarantees, exhaustive per code */
   for (n = 0; n < 20000; n++)
   {
      rand_ip(ip);
      inv_encode(ip, INV_DEFAULT_PORT, 1, code);
      typos(code, &singles[0], &swaps[0]);
      inv_encode(ip, 1 + rnd() % 65535, 1, code);
      if (inv_count(code) == 11)
         typos(code, &singles[1], &swaps[1]);
   }
   printf("     short form: single typos refused %lu/%lu, swaps refused %lu/%lu\n", singles[0],
          singles[0], swaps[0], swaps[0]);
   printf("     long form:  single typos refused %lu/%lu, swaps refused %lu/%lu\n", singles[1],
          singles[1], swaps[1], swaps[1]);
   /* 5. the rates: a random string that passes as an invite */
   {
      double r8 = rate_random(8, 2000000), r11 = rate_random(11, 2000000);
      printf("     random strings accepted: short %.3f%% (1/256 = 0.391%%, less impossible addresses), "
             "long %.3f%% (1/128 = 0.781%%)\n", r8, r11);
      CHECK(r8 < 0.45 && r11 < 0.85, "random acceptance too high");
   }
   /* the 32-bit bit-serial decoder against the first (64-bit, byte-wise) one */
   {
      unsigned long ok = 0, before = g_fail;
      same_as_ref(4000000ul, &ok);
      printf("     same as the reference decoder: 4000000 inputs (%lu valid), %s\n", ok,
             g_fail == before ? "identical" : "DIFFERENT");
   }
   printf("%s\n", g_fail ? "FAILED" : "PASS: invite codes");
   return g_fail ? 1 : 0;
}
