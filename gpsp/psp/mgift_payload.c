#include "mgift_payload.h"
#include <string.h>
static unsigned u16le(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return u16le(p) | ((uint32_t)u16le(p+2) << 16); }
static void put32(uint8_t *p, uint32_t v) {
   unsigned i; for (i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static uint32_t crc32(const uint8_t *p, unsigned n) {
   uint32_t c=0xffffffffu; unsigned i;
   while(n--) { c^=*p++; for(i=0;i<8;i++) c=(c>>1)^((c&1)?0xedb88320u:0); }
   return ~c;
}
int mgift_parcel_decode(mgift_parcel *out, const uint8_t *p, unsigned n) {
   unsigned fr, em, i, flag;
   if (!out || !p || n < MGIFT_HEADER_BYTES || memcmp(p,"MGC2",4)) return -1;
   fr=u16le(p+10); em=u16le(p+12);
   if (u16le(p+8)!=MGIFT_CARD_BYTES || u16le(p+14) ||
       fr>MGIFT_SCRIPT_MAX || em>MGIFT_SCRIPT_MAX || (!fr && !em) ||
       n!=MGIFT_HEADER_BYTES+MGIFT_CARD_BYTES+fr+em ||
       u32le(p+4)!=crc32(p+8,n-8)) return -1;
   flag=u16le(p+16);
   /* Current station catalogue: Aurora, Mystic and Emerald-only Old Sea Map. */
   if (flag<1000 || flag>1002 || (flag==1002 && fr)) return -1;
   if ((p[24]&3)!=0 || ((p[24]>>2)&15)>7 || (p[24]>>6)>2 || p[25]) return -1;
   for(i=0;i<8;i++) if(!memchr(p+26+40*i,0xff,40)) return -1;
   /* Relocatable entrypoint; full script semantics are verified by app tests. */
   if ((fr && (fr<6 || p[348]!=0xb8)) ||
       (em && (em<6 || p[348+fr]!=0xb8))) return -1;
   memset(out,0,sizeof(*out));
   out->id=u32le(p+4); out->frlg_size=(uint16_t)fr; out->emerald_size=(uint16_t)em;
   memcpy(out->card,p+16,MGIFT_CARD_BYTES);
   memcpy(out->frlg,p+348,fr); memcpy(out->emerald,p+348+fr,em);
   return 0;
}
void mgift_parcel_ack(uint8_t out[12], uint32_t id, unsigned result) {
   memcpy(out,"MGA2",4); put32(out+4,id); put32(out+8,result);
}
