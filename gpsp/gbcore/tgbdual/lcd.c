/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Copyright (C) 2001  Hii

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// 2005.05 major structure change by LCK

//-------------------------------------------------------
// LCD エミュレーション部
// inline assembler あり 適宜変更せよ

/* GBAdhoc: OSLib and MasterBoy headers replaced by gb.h/tgb_port.h.  The DMG
 * palette (MasterBoy's defaultPalCache/currentPalCache, RGB8888 from its
 * palette files) comes from the adapter as twelve BGR555 colours:
 * tgb_dmg_palette[], BG then OBJ0 then OBJ1. */
#include "gb.h"

word m_pal16[2][3][4] = {
	{	//GB
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0} },

	{	//SGB
		{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}
	}
};
word col_pal[16][4];
//Couleurs invalid馥s
byte inval_col_pal[16][4];
byte col_pal_is_invalidated = 1;
//Couleurs converties
word real_col_pal[16][4];
unsigned char gbc_cram_expand_table[32];

/* GBAdhoc: TGB_COLORIT guards MasterBoy's ColorIt (per-tile DMG
 * colourisation driven by palette files); it is not built. */
#ifdef TGB_COLORIT
byte tilePalList[384];
word tilePalPalette[NB_PALETTES][4];
word tilePalPalette_real[NB_PALETTES][4];
byte tilePalPalette_invalid[NB_PALETTES];
byte tilePalCrc[384 / 8];
//16 bytes per tile
//byte tileMapAltVram[384 * 16];
short tileMapTileList[384];
#endif
byte gb_pal_is_invalidated = 1;

int n_mpal16;
int now_win_line;
int mul;
int sprite_count;

char layer_enable[3];

byte ztbls[76];
//旧trans_tbl
#define ztbl1 (ztbls+12)
//旧trans_tbl&&priority_tbl
#define ztbl2 (ztbls+44)

static void gbc_recreate_colors();
static void sgb_recreate_colors();
void gb_invalidate_all_colors();
void lcd_bg_render_colored(void *buf,int scanline);
void lcd_win_render_colored(void *buf,int scanline);
void lcd_sprite_render_colored(void *buf,int scanline);
static void gb_recreate_colors();
void gb_invalidate_all_colors();
void gb_invalidate_palette(int palNo);

void lcd_init(void)
{
	n_mpal16 = 0;

	int i;
    for(i = 0; i < 32; i++)
    {
        unsigned char c = i << 3 | i >> 2;
        gbc_cram_expand_table[i] = c;
    }

	lcd_reset();
}

void lcd_set_mpal(int n)
{
	n_mpal16 = n;
}

int lcd_get_mpal()
{
	return n_mpal16;
}

void lcd_set_enable(int layer,char enable)
{
	layer_enable[layer]=enable;
}

char lcd_get_enable(int layer)
{
	return layer_enable[layer];
}

void lcd_reset()
{
	now_win_line=0;
	layer_enable[0]=layer_enable[1]=layer_enable[2]=true;
	sprite_count=0;
	gb_invalidate_all_colors();
}


/* - LCK
wordの画像データは２バイト１組で８ドット分を表している
１ドットは２ビットで表され、パレット番号を指定する

  addr 0   addr 1
  76543210 fedcba98     (ビット番号)
   ↓
  dot0  dot1  dot2  dot3  dot4  dot5  dot6  dot7
  f7    e6    d5    c4    b3    a2    91    80     (ビット番号)

資料にあたっておらずコードを読んだ結果なのでウソかも。原典にあたるべし。
*/

static const int sgb_palette_table[]={
	0x00, 0x01, 0x02, 0x03,
	0x00, 0x05, 0x06, 0x07,
	0x00, 0x09, 0x0A, 0x0B,
	0x00, 0x0D, 0x0E, 0x0F,
	0x00, 0x11, 0x12, 0x13,
	0x00, 0x15, 0x16, 0x17,
	0x00, 0x19, 0x1A, 0x1B,
	0x00, 0x1D, 0x1E, 0x1F,
	0x00, 0x21, 0x22, 0x23,
	0x00, 0x25, 0x26, 0x27,
	0x00, 0x29, 0x2A, 0x2B,
	0x00, 0x2D, 0x2E, 0x2F,
	0x00, 0x31, 0x32, 0x33,
	0x00, 0x35, 0x36, 0x37,
	0x00, 0x39, 0x3A, 0x3B,
	0x00, 0x3D, 0x3E, 0x3F,
	0x00, 0x41, 0x42, 0x43,
	0x00, 0x45, 0x46, 0x47,
	0x00, 0x49, 0x4A, 0x4B,
	0x00, 0x4D, 0x4E, 0x4F,
	0x00, 0x51, 0x52, 0x53,
	0x00, 0x55, 0x56, 0x57,
	0x00, 0x59, 0x5A, 0x5B,
	0x00, 0x5D, 0x5E, 0x5F,
	0x00, 0x61, 0x62, 0x63,
	0x00, 0x65, 0x66, 0x67,
	0x00, 0x69, 0x6A, 0x6B,
	0x00, 0x6D, 0x6E, 0x6F,
	0x00, 0x71, 0x72, 0x73,
	0x00, 0x75, 0x76, 0x77,
	0x00, 0x79, 0x7A, 0x7B,
	0x00, 0x7D, 0x7E, 0x7F
};

/*
//バイトデータのビット順を逆順にするためのテーブル
static const unsigned char tbl_bitrev[]={
	0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
	0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
	0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
	0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
	0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
	0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
	0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
	0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF,
};
//wordの画像データを水平方向に反転する
#define horizflip(src) (tbl_bitrev[src&0xff]|(tbl_bitrev[(src>>8)&0xff]<<8))
*/

static inline word horizflip(word src)
{
#ifdef __psp__
	/* GBAdhoc: the original wrote its input operand (bitrev %1,%1), which
	 * GCC's asm contract forbids; this form reads src and writes only ret. */
	dword ret;
	asm volatile(
		"		bitrev %0, %1"		"\n"
		"		wsbw %0, %0"		"\n"
			:	"=&r"(ret)
			:	"r"((dword)src)
	);
	return (word)ret;
#else
	/* GBAdhoc: portable form (Allegrex bitrev/wsbw) for host builds:
	 * reverse the bits of each byte of the 2bpp tile row. */
	unsigned lo = src & 0xff, hi = (src >> 8) & 0xff;
	lo = ((lo & 0xf0) >> 4) | ((lo & 0x0f) << 4);
	lo = ((lo & 0xcc) >> 2) | ((lo & 0x33) << 2);
	lo = ((lo & 0xaa) >> 1) | ((lo & 0x55) << 1);
	hi = ((hi & 0xf0) >> 4) | ((hi & 0x0f) << 4);
	hi = ((hi & 0xcc) >> 2) | ((hi & 0x33) << 2);
	hi = ((hi & 0xaa) >> 1) | ((hi & 0x55) << 1);
	return (word)(lo | (hi << 8));
#endif
}

static inline dword dtwk(dword src)
{
	dword d=src&0x0202;
	return ((d>>7)|d)&6;
}

void dattrans2n(word *pal, word *dat, dword src)
{
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>6) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>5) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>4) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>3) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>2) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src>>1) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src   ) ) );
	*(dat++)=*(word *)( ((byte *)pal)+( dtwk(src<<1) ) );
}

#define dattrans2nd(pal,dat,src) dattrans2n(pal,dat,src);dat+=8;

#define trb(ofs,b,c) if ( (!(b))&&(c) ) *(now_pos+ofs)=*(word *)(((char *)cur_p)+(c))

//screenxのtrans maskをztbl1に書き込み。priorityはtrue/falseでtransをマスクしてztbl2に
void set_ztbl(int screenx, byte mask, byte prio)
{
	// left byte right shift  (8-(screenx&7))
	// right byte left shift  (screenx&7)
	prio=((prio?0xff:0)&mask);
	ztbl1[(screenx>>3)  ] = ( ztbl1[(screenx>>3)  ] & (0xff<<(8-(screenx&7))) ) | (mask>>(screenx&7))     ;
	ztbl2[(screenx>>3)  ] = ( ztbl2[(screenx>>3)  ] & (0xff<<(8-(screenx&7))) ) | (prio>>(screenx&7))     ;
	ztbl1[(screenx>>3)+1] = ( ztbl1[(screenx>>3)+1] & (0xff>>(  (screenx&7))) ) | (mask<<(8-(screenx&7))) ;
	ztbl2[(screenx>>3)+1] = ( ztbl2[(screenx>>3)+1] & (0xff>>(  (screenx&7))) ) | (prio<<(8-(screenx&7))) ;
}

static inline byte get_ztbl1(int screenx)
{
	return ((ztbl1[screenx>>3]<<8)|ztbl1[(screenx>>3)+1])>>(8-(screenx&7));
}

static inline byte get_ztbl2(int screenx)
{
	return ((ztbl2[screenx>>3]<<8)|ztbl2[(screenx>>3)+1])>>(8-(screenx&7));
}

void lcd_bg_render(void *buf,int scanline)
{
	word pal[4];
	int t;

	pal[0]=m_pal16[n_mpal16][0][g_regs.BGP&0x3];
	pal[1]=m_pal16[n_mpal16][0][(g_regs.BGP>>2)&0x3];
	pal[2]=m_pal16[n_mpal16][0][(g_regs.BGP>>4)&0x3];
	pal[3]=m_pal16[n_mpal16][0][(g_regs.BGP>>6)&0x3];

	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x01)||
		(g_regs.WY<=(dword)scanline&&g_regs.WX<8&&(g_regs.LCDC&0x20))){
		if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x01)){
			word *tmp_w=(word*)buf+SIZE_LINE*scanline+GUARD_LINE;
			word tmp_dat=pal[0];
			for (t=0;t<160;t++)
				*(tmp_w++)=tmp_dat;
//			memset(((word*)buf)+160*scanline,0xff,160*2);
		}
		return;
	}

	word back=(g_regs.LCDC&0x08)?0x1C00:0x1800;
	word pat=(g_regs.LCDC&0x10)?0x0000:0x1000;
	word share=0x0000;//prefix
	byte tile;
	int i,x,y;
	byte *vrams[2]={vram,vram+0x2000};

	y=scanline+g_regs.SCY;
	if (y>=256)
		y-=256;
	x=g_regs.SCX;

	word *dat=((word*)buf)+scanline*SIZE_LINE+GUARD_LINE;

	int start=g_regs.SCX>>3;
	//int end=(start+20>32)?32:(start+21);
	int prefix=0;
	byte *now_tile=vrams[0]+back+((y>>3)<<5)+start;
	word *now_share=(word*)(vrams[0]+share+((y&7)<<1));
	word *now_pat=(word*)(vrams[0]+pat+((y&7)<<1));
//	word *now_share2=(word*)(vrams[0]+share+14-((y&7)<<1));
//	word *now_pat2=(word*)(vrams[0]+pat+14-((y&7)<<1));
	dword tmp_dat;

	int screenx=g_regs.SCX/8*8-g_regs.SCX;		//現在のスクリーン上でのｘ座標。負になることもあり。以下では[-7,160]を取り得る。
	dat+=screenx;

//	for (i=0;i<20;i++){
	for (i=0; ;i++){
		tile=*(now_tile++);
		tmp_dat=(tile&0x80)?*(now_share+(tile<<3)):*(now_pat+(tile<<3));
		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,1);
		screenx+=8;
		if (screenx>=160) break;

		if ((x/8*8+i*8)-prefix>=248){
			now_tile=vram+back+((y/8)<<5);
			prefix=256;
		}
	}
}


void lcd_win_render(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x20)||g_regs.WY>=(scanline+1)||g_regs.WX>166){
//		if ((g_regs.WY>=(scanline+1))&&((g_regs.LCDC&0x21)!=0x21))
//			memset(((word*)buf)+160*scanline,0,160*2);
		return;
	}

	int y=now_win_line-1/*scanline-res->system_reg.WY*/;
	now_win_line++;

	word back=(g_regs.LCDC&0x40)?0x1C00:0x1800;
	word pat=(g_regs.LCDC&0x10)?0x0000:0x1000;
	word share=0x0000;//prefix
	word pal[4];
	word *dat=(word*)buf;
	byte tile;
	int i;

	pal[0]=m_pal16[n_mpal16][0][g_regs.BGP&0x3];
	pal[1]=m_pal16[n_mpal16][0][(g_regs.BGP>>2)&0x3];
	pal[2]=m_pal16[n_mpal16][0][(g_regs.BGP>>4)&0x3];
	pal[3]=m_pal16[n_mpal16][0][(g_regs.BGP>>6)&0x3];
	dat+=SIZE_LINE*scanline+g_regs.WX-7+GUARD_LINE;
	byte *now_tile=vram+back+(((y>>3)-1)<<5);
	word *now_share=(word*)(vram+share+((y&7)<<1));
	word *now_pat=(word*)(vram+pat+((y&7)<<1));
	dword tmp_dat;

	int screenx=g_regs.WX-7;		//現在のスクリーン上でのｘ座標

	for (i=g_regs.WX>>3;i<21;i++){
		tile=*(now_tile++);
		tmp_dat=(tile&0x80)?*(now_share+(tile<<3)):*(now_pat+(tile<<3));
		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,1);
		screenx+=8;
	}
}

void lcd_sprite_render(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x02))
		return;

	word *sdat=((word*)buf)+(scanline)*SIZE_LINE+GUARD_LINE,*now_pos;
	int x,y,tile,atr,i,now;
	word tmp_dat;
	word pal[2][4],*cur_p;

	char sp_size=(g_regs.LCDC&0x04)?true:false;
	int palnum;

	pal[0][0]=m_pal16[n_mpal16][1][g_regs.OBP1&0x3];
	pal[0][1]=m_pal16[n_mpal16][1][(g_regs.OBP1>>2)&0x3];
	pal[0][2]=m_pal16[n_mpal16][1][(g_regs.OBP1>>4)&0x3];
	pal[0][3]=m_pal16[n_mpal16][1][(g_regs.OBP1>>6)&0x3];

	pal[1][0]=m_pal16[n_mpal16][2][g_regs.OBP2&0x3];
	pal[1][1]=m_pal16[n_mpal16][2][(g_regs.OBP2>>2)&0x3];
	pal[1][2]=m_pal16[n_mpal16][2][(g_regs.OBP2>>4)&0x3];
	pal[1][3]=m_pal16[n_mpal16][2][(g_regs.OBP2>>6)&0x3];

	for (i=39;i>=0;i--){
		tile=oam[i*4+2];
		atr=oam[i*4+3];
		palnum=(atr>>4)&1;
		cur_p=pal[palnum];

		if (sp_size){ // 8*16
			y=oam[i*4]-1;
			x=oam[i*4+1]-8;
			if ((x==-8&&y==-16)||x>SIZE_LINE_CLIP||y>144+15||(y<scanline)||(y>scanline+15))
				continue;
			if (scanline-y+15<8){
				now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
				tmp_dat=*(word*)(vram+(tile&0xfe)*16+now*2+((atr&0x40)?16:0));
			}
			else{
				now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
				tmp_dat=*(word*)(vram+(tile&0xfe)*16+now*2+((atr&0x40)?0:16));
			}
		}
		else{
			y=oam[i*4]-9;
			x=oam[i*4+1]-8;
			if ((x==-8&&y==-16)||(x>SIZE_LINE_CLIP)||(y>144+7)||(y<scanline)||(y>scanline+7))
				continue;
			now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
			tmp_dat=*(word*)(vram+tile*16+now*2);
		}
		sprite_count++;
		now_pos=sdat+x;

		if (atr&0x20) tmp_dat=horizflip(tmp_dat);	// 水平反転する。spriteはGBでも反転できるの？ -LCK

		byte zt=((atr&0x80)?get_ztbl1(x):0);
		trb(0,zt&0x80,dtwk(tmp_dat>>6));
		trb(1,zt&0x40,dtwk(tmp_dat>>5));
		trb(2,zt&0x20,dtwk(tmp_dat>>4));
		trb(3,zt&0x10,dtwk(tmp_dat>>3));
		trb(4,zt&0x08,dtwk(tmp_dat>>2));
		trb(5,zt&0x04,dtwk(tmp_dat>>1));
		trb(6,zt&0x02,dtwk(tmp_dat   ));
		trb(7,zt&0x01,dtwk(tmp_dat<<1));
	}
}

void lcd_bg_render_color(void *buf,int scanline)
{
	int t;

	// カラーではOFF機能が働かない?(僕のキャンプ場､モンコレナイト)
	if (!(g_regs.LCDC&0x80)/*||!(g_regs.LCDC&0x01)*/||
		(g_regs.WY<=(dword)scanline&&g_regs.WX<8&&(g_regs.LCDC&0x20))){
		if (!(g_regs.LCDC&0x80)/*||!(g_regs.LCDC&0x01)*/){
			word *tmp_w=(word*)buf+SIZE_LINE*scanline+GUARD_LINE;
			word tmp_dat=0x7fff;
			for (t=0;t<160;t++)
				*(tmp_w++)=tmp_dat;
//			memset(()+160*scanline,0xff,160*2);
		}
		goto fin;	//		return;
	}

	word back=(g_regs.LCDC&0x08)?0x1C00:0x1800;
	word pat=(g_regs.LCDC&0x10)?0x0000:0x1000;
	word share=0x0000;//prefix
	word *pal;
	byte tile;
	int i,x,y;
	byte *vrams[2]={vram,vram+0x2000};

	y=scanline+g_regs.SCY;
	if (y>=256)
		y-=256;
	x=g_regs.SCX;

	word *dat=((word*)buf)+scanline*SIZE_LINE+GUARD_LINE;

	int start=g_regs.SCX>>3;
	//int end=(start+20>32)?32:(start+21);
	int prefix=0;
	byte *now_tile  =vrams[0]+back+((y>>3)<<5)+start;
	byte *now_atr   =vrams[0]+back+((y>>3)<<5)+start+0x2000;
//	word *now_share =(word*)(vrams[0]+share+((y&7)<<1));
//	word *now_pat   =(word*)(vrams[0]+pat  +((y&7)<<1));
//	word *now_share2=(word*)(vrams[0]+share+14-((y&7)<<1));
//	word *now_pat2  =(word*)(vrams[0]+pat  +14-((y&7)<<1));
	dword tmp_dat;
	byte atr;

	int screenx=g_regs.SCX/8*8-g_regs.SCX;		//現在のスクリーン上でのｘ座標。負になることもあり。以下では[-7,160]を取り得る。
	dat+=screenx;

//	for (i=0;i<21;i++){
	for (i=0; ; i++){
		tile=*(now_tile++);
		atr=*(now_atr++);
		pal=real_col_pal[atr&7];
		tmp_dat=*(
			(word *)(
				vrams[0]
				+( (tile&0x80) ? share : pat )
				+( (atr&0x40)  ? (14-((y&7)<<1)) : ((y&7)<<1) )
			)
			+(tile<<3)				//tile
			+((atr<<9)&0x1000)		//bank
		);

		if (atr&0x20) tmp_dat=horizflip(tmp_dat);	// 水平反転する
		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,(atr&0x80));
		screenx+=8;
		if (screenx>=160) break;

		if ((x/8*8+i*8)-prefix>=248){
			now_tile=vram+back+((y/8)<<5);
			now_atr=vram+back+((y/8)<<5)+0x2000;
			prefix=256;
		}
	}
fin:
	// 多分こういうこと(僕のキャンプ場)
	if (!(g_regs.LCDC&0x01)) {
		memset(ztbls,0,sizeof(ztbls));
	}
}



void lcd_win_render_color(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x20)||g_regs.WY>=(scanline+1)||g_regs.WX>166){
//		if ((g_regs.WY>=(scanline+1))&&((g_regs.LCDC&0x21)!=0x21))
//			memset(((word*)buf)+160*scanline,0,160*2);
		return;
	}

	int y=now_win_line-1/*scanline-res->system_reg.WY*/;
	now_win_line++;

	byte *vrams[2]={vram,vram+0x2000};

	word back=(g_regs.LCDC&0x40)?0x1C00:0x1800;
	word pat=(g_regs.LCDC&0x10)?0x0000:0x1000;
	word share=0x0000;//prefix
	word *pal;
	word *dat=(word*)buf;
	byte tile;
	int i;

	dat+=SIZE_LINE*scanline+g_regs.WX-7+GUARD_LINE;

	byte *now_tile=vram+back+(((y>>3)-1)<<5);
	byte *now_atr=vram+back+(((y>>3)-1)<<5)+0x2000;
	word *now_share=(word*)(vram+share+((y&7)<<1));
	word *now_pat=(word*)(vram+pat+((y&7)<<1));
	word *now_share2=(word*)(vrams[0]+share+14-((y&7)<<1));
	word *now_pat2=(word*)(vrams[0]+pat+14-((y&7)<<1));
	dword tmp_dat;
	byte atr;
	word bank;

	int screenx=g_regs.WX-7;		//現在のスクリーン上でのｘ座標

	for (i=g_regs.WX>>3;i<21;i++){
		tile=*(now_tile++);
		atr=*(now_atr++);
		bank=(atr<<9)&0x1000;
		pal=real_col_pal[atr&7];
		tmp_dat=(tile&0x80)?*(((atr&0x40)?now_share2:now_share)+(tile<<3)+bank):*(((atr&0x40)?now_pat2:now_pat)+(tile<<3)+bank);

		if (atr&0x20) tmp_dat=horizflip(tmp_dat);	// 水平反転する
		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,(atr&0x80));
		screenx+=8;
	}
}

void lcd_sprite_render_color(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x02))
		return;

	ztbl1[-1]=ztbl1[20]=0;
	ztbl2[-1]=ztbl2[20]=0;

	word *sdat=((word*)buf)+(scanline)*SIZE_LINE+GUARD_LINE,*now_pos;
	int x,y,tile,atr,i,now;
	word tmp_dat;
	word *cur_p;

	char sp_size=(g_regs.LCDC&0x04)?true:false;

	word bank;

	for (i=39;i>=0;i--){
		if (sp_size){ // 8*16
			y=oam[i<<2]-1;
			if (((y-scanline)&-16)!=0) continue;			//縦方向の範囲判定
		} else { // 8*8
			y=oam[i<<2]-9;
			if (((y-scanline)&-8)!=0) continue;				//縦方向の範囲判定
		}
		x=oam[(i<<2)+1]-8;
		if (x<=-8||x>SIZE_LINE_CLIP) continue;					//横方向の範囲判定
//		if ((x==-8&&y==-16)||y>144+15) continue;			//その他の範囲判定

		tile=oam[(i<<2)+2];
		atr=oam[(i<<2)+3];
		cur_p=real_col_pal[(atr&7)+8];
		bank=(atr&0x08?0x2000:0);

		if (sp_size){ // 8*16
			if (scanline-y+15<8){ //上半分
				now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
				tmp_dat=*(word*)(vram+bank+((tile&0xfe)<<4)+(now<<1)+((atr&0x40)?16:0));
			}
			else{ // 下半分
				now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
				tmp_dat=*(word*)(vram+bank+((tile&0xfe)<<4)+(now<<1)+((atr&0x40)?0:16));
			}
		}
		else{ // 8*8
			now=(atr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
			tmp_dat=*(word*)(vram+(tile<<4)+(now<<1)+bank);
		}
		sprite_count++;
		now_pos=sdat+x; // now_pos=現在地点

		if (atr&0x20) tmp_dat=horizflip(tmp_dat);	// 水平反転する

		byte zt=((atr&0x80)?get_ztbl1(x):get_ztbl2(x));
		trb(0,zt&0x80,dtwk(tmp_dat>>6));
		trb(1,zt&0x40,dtwk(tmp_dat>>5));
		trb(2,zt&0x20,dtwk(tmp_dat>>4));
		trb(3,zt&0x10,dtwk(tmp_dat>>3));
		trb(4,zt&0x08,dtwk(tmp_dat>>2));
		trb(5,zt&0x04,dtwk(tmp_dat>>1));
		trb(6,zt&0x02,dtwk(tmp_dat   ));
		trb(7,zt&0x01,dtwk(tmp_dat<<1));
	}
}

static void lcd_sgb_render(void *buf,int scanline)
{
	int i;
	const int *tbl;
	word *sdat=((word*)buf)+(scanline)*SIZE_LINE+GUARD_LINE;
	byte *atf = &sgb_ATF[(scanline/8)*20];

	for(i=0; i<20; i++){
		tbl = &sgb_palette_table[(*(atf++))<<2];
		/*
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*sdat = sgb_palette[tbl[*sdat]]; sdat++;
		*/
		sdat[0] = sgb_palette_real[tbl[sdat[0]]];
		sdat[1] = sgb_palette_real[tbl[sdat[1]]];
		sdat[2] = sgb_palette_real[tbl[sdat[2]]];
		sdat[3] = sgb_palette_real[tbl[sdat[3]]];
		sdat[4] = sgb_palette_real[tbl[sdat[4]]];
		sdat[5] = sgb_palette_real[tbl[sdat[5]]];
		sdat[6] = sgb_palette_real[tbl[sdat[6]]];
		sdat[7] = sgb_palette_real[tbl[sdat[7]]];
		sdat+=8;
	}
}

void lcd_render(void *buf,int scanline)
{
	sprite_count=0;

	switch(now_gb_mode)
	{
	case 3:
		gbc_recreate_colors();
		lcd_bg_render_color(buf,scanline);
		lcd_win_render_color(buf,scanline);
		lcd_sprite_render_color(buf,scanline);
		break;

	case 2:
//		n_mpal16 = PAL_SGB;

		/* GBAdhoc: vframe is ordinary cached memory here; MasterBoy's
		 * oslGetCachedPtr/oslUncacheData pair (below, removed) mapped its
		 * uncached VRAM frame. */
		//Pour le SGB comme on fera plusieurs acc鑚 (dont un r/w dans lcd_sgb_render), le cache vaut la peine
		sgb_recreate_colors();
		lcd_bg_render(buf,scanline);
		lcd_win_render(buf,scanline);
		lcd_sprite_render(buf,scanline);
		lcd_sgb_render(buf,scanline);

		//Last line: MasterBoy uncached its VRAM frame here (removed).
		break;

	case 1:
		gb_recreate_colors();
		lcd_bg_render(buf,scanline);
		lcd_win_render(buf,scanline);
		lcd_sprite_render(buf,scanline);
		break;
	}
}

void lcd_clear_win_count()
{
	now_win_line=9;
}

word *lcd_get_pal(int num)
{
	return col_pal[num];
}

void gbc_invalidate_color(int palno, int color)		{
	col_pal_is_invalidated = inval_col_pal[palno][color] = 1;
}

void sgb_invalidate_color(int color)		{
	sgb_palette_invalidated = sgb_palette_inval[color] = 1;
}

void sgb_set_color(int color, word value)		{
	sgb_palette_invalidated = sgb_palette_inval[color] = 1;
	sgb_palette[color] = value;
}

/* GBAdhoc: the loops below walk 2-D arrays as flat ones.  Indexing past the
 * inner dimension (inval_col_pal[0][i] for i >= 4) is undefined, and GCC
 * warns it may cut such a loop to four iterations -- which would leave most
 * CGB palettes and both DMG sprite palettes unconverted.  Flat pointers
 * express the same walk legally. */
static void gbc_recreate_colors()		{
	int i;
	if (col_pal_is_invalidated)				{
		byte *inval = &inval_col_pal[0][0];
		word *src = &col_pal[0][0], *dst = &real_col_pal[0][0];
		for (i=0;i<16*4;i++)		{
			if (inval[i])		{
				word col = src[i];
				dst[i] = CONVERT_COLOR15(col);
				inval[i] = 0;
			}
		}
		col_pal_is_invalidated = 0;
	}
}

void gb_invalidate_palette(int palNo)		{
#ifdef TGB_COLORIT
	tilePalPalette_invalid[palNo] = 1;
#endif
	gb_pal_is_invalidated = 1;
}

static void gb_recreate_colors()		{
	int i;
	if (gb_pal_is_invalidated)				{
#ifdef TGB_COLORIT
		int j;
		if (gblModeColorIt)		{
			for (j=0;j<NB_PALETTES;j++)		{
				if (tilePalPalette_invalid[j])		{
					for (i=0;i<4;i++)		{
						word col = tilePalPalette[j][i];
						tilePalPalette_real[j][i] = CONVERT_COLOR15(col);
					}
					tilePalPalette_invalid[j] = 0;
				}
			}
		}
#endif

		//Palette pour le mode noir & blanc - pas besoin de le faire toujours mais
		word *dmg = &m_pal16[0][0][0];	/* GBAdhoc: flat, see gbc_recreate_colors */
		for (i=0;i<12;i++)		{
			word col = tgb_dmg_palette[i];	/* GBAdhoc: BGR555 from the adapter */
			dmg[i] = CONVERT_COLOR15(col);
		}

		gb_pal_is_invalidated = 0;
	}
}


static void sgb_recreate_colors()		{
	int i;
	if (sgb_palette_invalidated)		{
		for (i=0;i<16*8;i++)		{
			if (sgb_palette_inval[i])		{
				word col = sgb_palette[i];
				sgb_palette_real[i] = CONVERT_COLOR15(col);
				sgb_palette_inval[i] = 0;
			}
		}
		sgb_palette_invalidated = 0;
	}

}

void gb_invalidate_all_colors()		{
	int i;
	for (i=0;i<16*4;i++)
		gbc_invalidate_color(i>>2, i&3);	/* GBAdhoc: was (0, i) */
	for (i=0;i<16*8;i++)
		sgb_invalidate_color(i);
#ifdef TGB_COLORIT
	for (i=0;i<NB_PALETTES;i++)
		gb_invalidate_palette(i);
#else
	gb_invalidate_palette(0);
#endif
}

#ifdef TGB_COLORIT
void lcd_bg_render_colored(void *buf,int scanline)
{
	word pal[4];
	int t;

	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x01)||
		(g_regs.WY<=(dword)scanline&&g_regs.WX<8&&(g_regs.LCDC&0x20))){
		if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x01)){
			word *tmp_w=(word*)buf+SIZE_LINE*scanline+GUARD_LINE;
			word tmp_dat=pal[0];
			for (t=0;t<160;t++)
				*(tmp_w++)=tmp_dat;
//			memset(((word*)buf)+160*scanline,0xff,160*2);
		}
		return;
	}

	word back=(g_regs.LCDC&0x08)?0x1C00:0x1800;
	//Normalement c'est 0x1000 et 0x100 mais j'ai / 16 (taille d'une tile)
	word pat = (g_regs.LCDC&0x10)?0x000:0x100;
	word share=0x0000;//prefix
	byte tile;
	int i,x,y;
	byte *vrams[2]={vram,vram+0x2000};

	y=scanline+g_regs.SCY;
	if (y>=256)
		y-=256;
	x=g_regs.SCX;

	word *dat=((word*)buf)+scanline*SIZE_LINE+GUARD_LINE;

	int start=g_regs.SCX>>3;
	//int end=(start+20>32)?32:(start+21);
	int prefix=0;
	byte *now_tile=vrams[0]+back+((y>>3)<<5)+start;
//	word *now_share=(word*)(vrams[0]+share+((y&7)<<1));
//	word *now_pat=(word*)(vrams[0]+pat+((y&7)<<1));

//	word *now_share2=(word*)(vrams[0]+share+14-((y&7)<<1));
//	word *now_pat2=(word*)(vrams[0]+pat+14-((y&7)<<1));
	dword tmp_dat;

	int screenx=g_regs.SCX/8*8-g_regs.SCX;		//現在のスクリーン上でのｘ座標。負になることもあり。以下では[-7,160]を取り得る。
	dat+=screenx;

//	for (i=0;i<20;i++){
	for (i=0; ;i++){
		tile=*(now_tile++);
		//CODE ADDED FOR COLORATION
		int tilenb = (tile&0x80)?(/*share+*/tile):(pat+tile);
		tmp_dat = *(word*)(vram + (tileMapTileList[tilenb] << 4) + ((y&7)<<1));
		pal[0] = tilePalPalette_real[tilePalList[tilenb]][g_regs.BGP&0x3];
		pal[1] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>2)&0x3];
		pal[2] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>4)&0x3];
		pal[3] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>6)&0x3];

		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,1);
		screenx+=8;
		if (screenx>=160) break;

		if ((x/8*8+i*8)-prefix>=248){
			now_tile=vram+back+((y/8)<<5);
			prefix=256;
		}
	}
}

void lcd_win_render_colored(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x20)||g_regs.WY>=(scanline+1)||g_regs.WX>166){
//		if ((g_regs.WY>=(scanline+1))&&((g_regs.LCDC&0x21)!=0x21))
//			memset(((word*)buf)+160*scanline,0,160*2);
		return;
	}

	int y=now_win_line-1/*scanline-res->system_reg.WY*/;
	now_win_line++;

	word back=(g_regs.LCDC&0x40)?0x1C00:0x1800;
	word pat=(g_regs.LCDC&0x10)?0x000:0x100;
	word share=0x0000;//prefix
	word pal[4];
	word *dat=(word*)buf;
	byte tile;
	int i;

	dat+=SIZE_LINE*scanline+g_regs.WX-7+GUARD_LINE;
	byte *now_tile=vram+back+(((y>>3)-1)<<5);
	word *now_share=(word*)(vram+share+((y&7)<<1));
	word *now_pat=(word*)(vram+pat+((y&7)<<1));
	dword tmp_dat;

	int screenx=g_regs.WX-7;		//現在のスクリーン上でのｘ座標

	for (i=g_regs.WX>>3;i<21;i++){
		tile=*(now_tile++);
		//CODE ADDED FOR COLORATION
/*		word *adr = (tile&0x80)?(now_share+(tile<<3)):(now_pat+(tile<<3));
		tmp_dat = *adr;
		int tilenb = ((u32)adr - (u32)vram) >> 4;*/
		int tilenb = (tile&0x80)?(share+tile):(pat+tile);
		tmp_dat = *(word*)(vram + (tileMapTileList[tilenb] << 4) + ((y&7)<<1));

		pal[0] = tilePalPalette_real[tilePalList[tilenb]][g_regs.BGP&0x3];
		pal[1] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>2)&0x3];
		pal[2] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>4)&0x3];
		pal[3] = tilePalPalette_real[tilePalList[tilenb]][(g_regs.BGP>>6)&0x3];

		dattrans2nd(pal,dat,tmp_dat);
		set_ztbl(screenx,(tmp_dat>>8)|tmp_dat,1);
		screenx+=8;
	}
}

void lcd_sprite_render_colored(void *buf,int scanline)
{
	if (!(g_regs.LCDC&0x80)||!(g_regs.LCDC&0x02))
		return;

	word *sdat=((word*)buf)+(scanline)*SIZE_LINE+GUARD_LINE,*now_pos;
	int x,y,tile,atr,i,now;
	word tmp_dat;
//	word pal[2][4],*cur_p;
	word cur_p[4];

	char sp_size=(g_regs.LCDC&0x04)?true:false;
	int palnum;

	for (i=39;i>=0;i--){
		tile=oam[i*4+2];
		atr=oam[i*4+3];
		int oldatr = atr;
//		palnum=(atr>>4)&1;
//		cur_p=pal[palnum];

		if (sp_size){ // 8*16
			y=oam[i*4]-1;
			x=oam[i*4+1]-8;
			if ((x==-8&&y==-16)||x>SIZE_LINE_CLIP||y>144+15||(y<scanline)||(y>scanline+15))
				continue;

			if (scanline - y + 15 >= 8)
				atr ^= 0x40;

//			tile = (tile & 0xfe) + ((atr&0x40) ? 1 : 0);
			tile = (tile & 0xfe) + ((atr&0x40) >> 6);
		}
		else			{
			y=oam[i*4]-9;
			x=oam[i*4+1]-8;
			if ((x==-8&&y==-16)||(x>SIZE_LINE_CLIP)||(y>144+7)||(y<scanline)||(y>scanline+7))
				continue;
		}

		now=(oldatr&0x40)?((y-scanline)&7):((7-(y-scanline))&7);
		tmp_dat = ((word*)vram)[(tileMapTileList[tile] << 3) + now];
		sprite_count++;
		now_pos=sdat+x;

		if (atr&0x20) tmp_dat=horizflip(tmp_dat);	// 水平反転する。spriteはGBでも反転できるの？ -LCK

		if ((atr>>4) & 1)		{
			cur_p[0] = tilePalPalette_real[tilePalList[tile]][g_regs.OBP2&0x3];
			cur_p[1] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP2>>2)&0x3];
			cur_p[2] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP2>>4)&0x3];
			cur_p[3] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP2>>6)&0x3];
		}
		else	{
			cur_p[0] = tilePalPalette_real[tilePalList[tile]][g_regs.OBP1&0x3];
			cur_p[1] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP1>>2)&0x3];
			cur_p[2] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP1>>4)&0x3];
			cur_p[3] = tilePalPalette_real[tilePalList[tile]][(g_regs.OBP1>>6)&0x3];
		}

		byte zt=((atr&0x80)?get_ztbl1(x):0);
		trb(0,zt&0x80,dtwk(tmp_dat>>6));
		trb(1,zt&0x40,dtwk(tmp_dat>>5));
		trb(2,zt&0x20,dtwk(tmp_dat>>4));
		trb(3,zt&0x10,dtwk(tmp_dat>>3));
		trb(4,zt&0x08,dtwk(tmp_dat>>2));
		trb(5,zt&0x04,dtwk(tmp_dat>>1));
		trb(6,zt&0x02,dtwk(tmp_dat   ));
		trb(7,zt&0x01,dtwk(tmp_dat<<1));
	}
}

void tilePalReinit()		{
	int i;
	memset(tilePalList, 0, sizeof(tilePalList));
	for (i=0;i<oslNumberof(tileMapTileList);i++)
		tileMapTileList[i] = i;
}

void tilePalInit()		{
	memset(tilePalCrc, 0, sizeof(tilePalCrc));
	gb_lastVramCrc = 0;
}


/*void tilePalMapInit()		{
}*/

int tileMapGetVramChecksum()		{
	int i;
	u32 crc = 0;
	for (i=0;i<384;i++)		{
		if (tilePalCrc[i >> 3] & (1 << (i & 7)))
			crc = crc32(crc, vram + i * 16, 16);
	}
	return crc;
}
#endif /* TGB_COLORIT */
