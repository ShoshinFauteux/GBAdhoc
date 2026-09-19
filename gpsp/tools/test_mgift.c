/* Production cart/parser, host callback seam; run with ASan + UBSan.
 * Fixtures are emitted by Android GiftBundleTest, not a second encoder. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../psp/mgift_cart.c"
static unsigned injected, last_type;
static int in_send;
void fe_evt(const char *fmt, ...) { (void)fmt; }
static void receive(const void *p, size_t size, uint16_t peer) {
   assert(!in_send); assert(size>=12); assert(peer==0);
   injected++;last_type=up32be((const uint8_t *)p+4);
}
static void start(uint16_t id, retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll) {
   assert(id==1);assert(send==cart_core_send);assert(poll);
}
static void stop(void) {}
static const struct retro_netpacket_callback iface={start,receive,stop,NULL,NULL,NULL,"test"};
const void *fe_host_netpacket_cb(void) { return &iface; }
static unsigned fixture(const char *path, uint8_t *p) {
   FILE *f=fopen(path,"rb");unsigned n;assert(f);n=fread(p,1,MGIFT_PARCEL_MAX+1,f);fclose(f);return n;
}
static void message(unsigned type, unsigned header, const uint8_t *data, unsigned n) {
   uint8_t p[104]={0};pk32be(p,RFU1_MAGIC);pk32be(p+4,type);pk32be(p+8,header);
   assert(n<=92);if(n)memcpy(p+12,data,n);
   in_send=1;cart_core_send(0,p,sizeof(p),0);in_send=0;
}
static void game_data(const char *code, unsigned kind, unsigned version, unsigned existing) {
   uint8_t body[100]={0};
   memset(&C,0,sizeof(C));C.active=C.linked=C.have_card=1;
   memcpy(C.card,P.card,sizeof(C.card));
   C.svr=SV_RECV_GAMEDATA;mgl_expect(MG_LINKID_GAME_DATA);
   pk16le(body,0x101);body[4]=body[8]=1;pk16le(body+12,kind);
   pk16le(body+16,version);pk16le(body+20,existing);memcpy(body+92,code,4);
   pk16le(C.blk_rx,MG_LINKID_GAME_DATA);pk16le(C.blk_rx+2,mgl_crc16(body,sizeof(body)));
   pk16le(C.blk_rx+4,sizeof(body));C.blk_rx_frags=1;C.blk_rx_done=1;
   svr_step();assert(C.svr==SV_RECV_GAMEDATA);
   memcpy(C.blk_rx,body,sizeof(body));C.blk_rx_frags=9;C.blk_rx_done=1;svr_step();
}
static void toss_response(unsigned response) {
   uint8_t body[4]={0};body[0]=(uint8_t)response;
   C.svr=SV_RECV_TOSS;mgl_expect(MG_LINKID_RESPONSE);
   pk16le(C.blk_rx,MG_LINKID_RESPONSE);pk16le(C.blk_rx+2,mgl_crc16(body,4));
   pk16le(C.blk_rx+4,4);C.blk_rx_frags=1;C.blk_rx_done=1;svr_step();
   memcpy(C.blk_rx,body,4);C.blk_rx_done=1;svr_step();
}
int main(int argc,char **argv) {
   uint8_t bytes[MGIFT_PARCEL_MAX+1], copy[MGIFT_PARCEL_MAX+1], ack[12];
   mgift_parcel parcel, sentinel;unsigned n,i;
   assert(argc==2);n=fixture(argv[1],bytes);
   assert(!mgift_parcel_decode(&parcel,bytes,n));
   memset(&sentinel,0xa5,sizeof(sentinel));
   for(i=0;i<n;i++) { mgift_parcel out=sentinel;
      assert(mgift_parcel_decode(&out,bytes,i)<0);assert(!memcmp(&out,&sentinel,sizeof(out)));
      memcpy(copy,bytes,n);copy[i]^=1;assert(mgift_parcel_decode(&out,copy,n)<0);
   }
   memcpy(copy,bytes,n);copy[n]=0;assert(mgift_parcel_decode(&sentinel,copy,n+1)<0);
   mgift_parcel_ack(ack,parcel.id,1);assert(!memcmp(ack,"MGA2",4));assert(ack[8]==1);
   /* Independent known-answer vector: pret CalcCRC16WithTable includes ~crc. */
   assert(mgl_crc16(NULL,0)==0xeede);
   { const uint8_t script[]={8,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,2,0,0,0,16,0,0,0,4,0,0,0,0,0,0,0};
     assert(mgl_crc16(script,sizeof(script))==0xbb53); }
   assert(!mgift_cart_start());
   for(i=0;i<60;i++)mgift_cart_frame();
   assert(injected==0); /* no empty offer */
   assert(!mgift_cart_set_parcel(&parcel));
   message(NET_RFU_CONNECT_REQ,MGC_OUR_DEVID,NULL,0);
   assert(!injected && C.connect_pending && !C.linked);
   mgift_cart_frame();assert(injected==1 && last_type==NET_RFU_CONNECT_ACK);
   assert(!mgift_cart_set_parcel(&parcel)); /* duplicate cannot reset connection */
   assert(C.linked);sentinel=parcel;sentinel.id++;assert(mgift_cart_set_parcel(&sentinel)==1);
   /* The core's fixed 104-byte carrier contains a two-byte child NI header. */
   { uint8_t control[]={0x87,0x04,1,12,0,26,0,0,0};
     message(NET_RFU_CLIENT_SEND,(9u<<24)|MGC_CLIENT_DEVID,control,sizeof(control));
     assert(C.ni_ack==((1u<<18)|(1u<<14)|(1u<<13)|(1u<<11)));
     mgift_cart_frame();assert(!C.ni_ack && last_type==NET_RFU_HOST_SEND); }
   /* Reject all truncated subframes and out-of-range block counts/indices. */
   { uint8_t data[16]={0x0e,0x10,0xe0,0x88,1,0,0x81,0};
     for(i=0;i<16;i++)cart_receive_llsf(data,i);
     assert(!C.blk_rx_busy);
     cart_receive_llsf(data,16);assert(C.blk_rx_busy && C.blk_rx_frags==1);
     assert(C.echo[0]==0 && C.echo[1]==0x88);
     data[2]=31;data[3]=0x89;cart_receive_llsf(data,16);assert(!C.blk_rx_flags);
     data[2]=0;cart_receive_llsf(data,16);assert(C.blk_rx_done);
   }
   /* CRC/ident failure never advances to writing a card. */
   C.svr=SV_RECV_GAMEDATA;C.blk_rx_frags=1;C.blk_rx_done=1;
   pk16le(C.blk_rx,17);pk16le(C.blk_rx+2,0);pk16le(C.blk_rx+4,4);
   mgl_expect(17);assert(!mgl_receive());
   C.blk_rx_done=1;memset(C.blk_rx,0,12);assert(!mgl_receive());
   assert(C.svr==SV_FAILED && C.st.crc_fail==1);
   mgift_cart_frame();assert(!C.linked && last_type==NET_RFU_DISCONNECT);
   message(NET_RFU_CONNECT_REQ,MGC_OUR_DEVID,NULL,0);
   mgift_cart_frame();assert(C.linked && C.svr==SV_IDLE && !C.join_stage);
   C.frame += 601;mgift_cart_frame();assert(!C.linked && C.svr==SV_FAILED);
   /* Actual GAME_DATA layout: game code at 92, not the previously assumed 94.
    * Each supported game gets its own script and deterministic zero padding. */
   game_data("BPRE",1,1,0);
   assert(C.svr==(P.frlg_size ? SV_SEND_SAVECARD_SCRIPT : SV_SEND_RESULT));
   if(P.frlg_size) {
      assert(!memcmp(C.ram_script,P.frlg,P.frlg_size));
      for(i=P.frlg_size;i<sizeof(C.ram_script);i++)assert(C.ram_script[i]==0);
   } else assert(C.result==CLI_MSG_CANT_ACCEPT);
   game_data("BPGE",1,2,0);
   assert(C.svr==(P.frlg_size ? SV_SEND_SAVECARD_SCRIPT : SV_SEND_RESULT));
   game_data("BPEE",4,0x200,0);assert(C.svr==SV_SEND_SAVECARD_SCRIPT);
   assert(!memcmp(C.ram_script,P.emerald,P.emerald_size));
   for(i=P.emerald_size;i<sizeof(C.ram_script);i++)assert(C.ram_script[i]==0);
   game_data("BPRJ",1,1,0);assert(C.svr==SV_SEND_RESULT && C.result==CLI_MSG_CANT_ACCEPT);
   game_data("BPRE",1,2,0);assert(C.svr==SV_SEND_RESULT && C.result==CLI_MSG_CANT_ACCEPT);
   game_data("BPEE",4,0x200,up16le(P.card));
   assert(C.svr==SV_SEND_RESULT && C.result==CLI_MSG_HAD_CARD);
   game_data("BPEE",4,0x200,999);assert(C.svr==SV_SEND_TOSS_SCRIPT);
   toss_response(1);assert(C.svr==SV_SEND_RESULT && C.result==CLI_MSG_COMM_CANCELED);
   game_data("BPEE",4,0x200,999);toss_response(0);assert(C.svr==SV_SEND_SAVECARD_SCRIPT);
   /* Human prompts may take longer than the transfer timeout while polling. */
   C.svr=SV_RECV_TOSS;C.exchange=4;C.frame=2000;C.last_rx=2000;C.last_progress=0;
   mgift_cart_frame();assert(C.linked);
   mgift_cart_stop();puts("Mystery Gift protocol tests passed");return 0;
}
