/* invite_code.h -- the invite code: a desktop host's IPv4 address (and port)
 * with a check, so a PSP can join it with no server at all.  Header-only, no
 * allocation, no libc (the PSP module has none of its own).  The format is
 * the one the design mockup drew (builds/xmb-relay-mockup, data.js); the spec
 * is docs/ONLINE-TUNNEL.md, "Invite codes", the tests tools/relay/test_invite.c
 * (which also check vectors made by the mockup's own encoder).
 *
 * ALPHABET.  Crockford base32: 0-9 A-Z without I L O U.  Shown in groups of
 * four with dashes (XXXX-XXXX, XXXX-XXXX-XXX).  On input, case, dashes and
 * spaces do not matter, and the look-alikes are folded: O -> 0, I and L -> 1,
 * U -> V.
 *
 * SHORT FORM, 8 symbols = 40 bits, most significant first:
 *    bits 39..8   IPv4 a.b.c.d (a first)
 *    bits  7..0   check = CRC-8(a, b, c, d) ^ 0x5A
 *    the port is GBAdhoc's default, INV_DEFAULT_PORT (its probe port + 1).
 * LONG FORM, 11 symbols = 55 bits, for a host on another port:
 *    bits 54..23  IPv4,  bits 22..7  port,  bits 6..0  check =
 *    CRC-7(a, b, c, d, port_hi, port_lo) ^ 0x5A & 0x7F
 * CRC-8: polynomial 0x07; CRC-7: polynomial 0x09 (x^7 + x^3 + 1, primitive);
 * both initial value 0, no reflection, MSB first.  (The mockup drew the long
 * form's check as the low 7 bits of the CRC-8: that misses about 1 in 128
 * single-symbol typos, which test_invite.c found; a real CRC-7 misses none.)  0x5A
 * is the format's version tag: another tag makes every code of this version
 * fail the check rather than decode to a wrong address.
 *
 * What the check catches (measured exhaustively by test_invite.c): every
 * single wrong symbol, every swap of two different symbols, and all but
 * about 1 in 256 (short) / 1 in 128 (long) of other mistakes.  Decoding also
 * refuses addresses no host can have (0.x.x.x, 224.0.0.0 and up) and port 0.
 */
#ifndef INVITE_CODE_H
#define INVITE_CODE_H

#include <stdint.h>

#define INV_SHORT        8
#define INV_LONG         11
#define INV_DEFAULT_PORT 27420u          /* RL_DEFAULT_PORT */
#define INV_TAG          0x5Au

/* INV_ALPHABET_STORAGE: define it as plain `static` to put the table in .data
 * (an includer whose .rodata is layout-pinned, docs/LAYOUT-PINNING.md; the
 * EBOOT no longer includes this file). */
#ifndef INV_ALPHABET_STORAGE
#define INV_ALPHABET_STORAGE static const
#endif
INV_ALPHABET_STORAGE char INV_ALPHABET[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

enum { INV_OK = 0, INV_E_LENGTH = -1, INV_E_SYMBOL = -2, INV_E_CHECK = -3,
       INV_E_ADDRESS = -4, INV_E_PORT = -5 };

static inline unsigned inv_crc7(const uint8_t *b, int n)
{
   unsigned c = 0;
   int i, k;
   for (i = 0; i < n; i++)
      for (k = 7; k >= 0; k--)
      {
         unsigned fb = ((c >> 6) ^ (b[i] >> k)) & 1;
         c = (c << 1) & 0x7F;
         if (fb)
            c ^= 0x09;
      }
   return c;
}

static inline unsigned inv_crc8(const uint8_t *b, int n)
{
   unsigned c = 0;
   int i, k;
   for (i = 0; i < n; i++)
   {
      c ^= b[i];
      for (k = 0; k < 8; k++)
         c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xFF : (c << 1) & 0xFF;
   }
   return c;
}

/* one character -> its symbol value; -1 = refused, -2 = a separator */
static inline int inv_sym(char ch)
{
   int i;
   if (ch == '-' || ch == ' ')
      return -2;
   if (ch >= 'a' && ch <= 'z')
      ch = (char)(ch - 32);
   if (ch == 'O')
      ch = '0';
   else if (ch == 'I' || ch == 'L')
      ch = '1';
   else if (ch == 'U')
      ch = 'V';
   for (i = 0; i < 32; i++)
      if (INV_ALPHABET[i] == ch)
         return i;
   return -1;
}

static inline int inv_ip_ok(const uint8_t ip[4])
{
   return ip[0] != 0 && ip[0] < 224;
}

/* ip[4] in network order, port = the relay's main port (the default gives
 * the short form).  dash = groups of four.  out: cap >= 14. */
static inline int inv_encode(const uint8_t ip[4], unsigned port, int dash, char *out)
{
   uint64_t v;
   uint8_t b[6];
   int n, i, o = 0;
   if (!inv_ip_ok(ip))
      return INV_E_ADDRESS;
   if (!port || port > 65535)
      return INV_E_PORT;
   b[0] = ip[0]; b[1] = ip[1]; b[2] = ip[2]; b[3] = ip[3];
   v = ((uint64_t)ip[0] << 24) | ((uint64_t)ip[1] << 16) | ((uint64_t)ip[2] << 8) | ip[3];
   if (port == INV_DEFAULT_PORT)
   {
      v = (v << 8) | ((inv_crc8(b, 4) ^ INV_TAG) & 0xFF);
      n = INV_SHORT;
   }
   else
   {
      b[4] = (uint8_t)(port >> 8);
      b[5] = (uint8_t)port;
      v = (((v << 16) | port) << 7) | ((inv_crc7(b, 6) ^ INV_TAG) & 0x7F);
      n = INV_LONG;
   }
   for (i = 0; i < n; i++)
   {
      if (dash && i && i % 4 == 0)
         out[o++] = '-';
      out[o++] = INV_ALPHABET[(v >> (5 * (n - 1 - i))) & 31];
   }
   out[o] = 0;
   return INV_OK;
}

/* The number of code symbols in s (separators skipped), or -1 when s holds a
 * character that is no symbol.  4 = a room code, 8 or 11 = an invite. */
static inline int inv_count(const char *s)
{
   int n = 0, v;
   for (; *s; s++)
   {
      v = inv_sym(*s);
      if (v == -1)
         return -1;
      if (v >= 0)
         n++;
   }
   return n;
}

/* Decoding is bit-serial and 32-bit only: 64-bit shifts plus two inlined byte
 * CRCs cost ~1 KB of MIPS text.  (The PSP now checks invites in the online
 * module, not the EBOOT, whose boot-heap rule that broke: docs/ONLINE-TUNNEL.md.)  A CRC fed one
 * bit at a time MSB first equals the byte-wise inv_crc8 / inv_crc7 above (both
 * start from 0 and are not reflected); test_invite checks every vector,
 * single typo and swap against the encoder. */
static inline int inv_decode(const char *s, uint8_t ip[4], unsigned *port)
{
   uint8_t q[INV_LONG];
   uint32_t a = 0, t = 0;       /* a: the address bits; t: every bit after them */
   unsigned c = 0, top, mask, poly, p = INV_DEFAULT_PORT;
   int n = 0, x, j, k, i = 0, nd;
   for (; *s; s++)
   {
      x = inv_sym(*s);
      if (x == -2)
         continue;
      if (x < 0)
         return INV_E_SYMBOL;
      if (n == INV_LONG)
         return INV_E_LENGTH;
      q[n++] = (uint8_t)x;
   }
   if (n != INV_SHORT && n != INV_LONG)
      return INV_E_LENGTH;
   /* short: CRC-8 (0x07) over 32 address bits; long: CRC-7 (0x09) over 48 */
   top = n == INV_SHORT ? 0x80u : 0x40u;
   mask = top * 2 - 1;
   poly = n == INV_SHORT ? 0x07u : 0x09u;
   nd = n == INV_SHORT ? 32 : 48;
   for (j = 0; j < n; j++)
      for (k = 4; k >= 0; k--, i++)
      {
         unsigned b = ((unsigned)q[j] >> k) & 1;
         if (i < nd)
         {
            unsigned fb = (c & top) ? 1 ^ b : b;
            c = (c << 1) & mask;
            if (fb)
               c ^= poly;
         }
         if (i < 32)
            a = (a << 1) | b;
         else
            t = (t << 1) | b;
      }
   if (n == INV_LONG)
      p = (unsigned)(t >> 7);
   ip[0] = (uint8_t)(a >> 24);
   ip[1] = (uint8_t)(a >> 16);
   ip[2] = (uint8_t)(a >> 8);
   ip[3] = (uint8_t)a;
   if (((c ^ INV_TAG) & mask) != (t & mask))
      return INV_E_CHECK;
   if (!inv_ip_ok(ip))
      return INV_E_ADDRESS;
   if (!p)
      return INV_E_PORT;
   *port = p;
   return INV_OK;
}

#endif /* INVITE_CODE_H */
