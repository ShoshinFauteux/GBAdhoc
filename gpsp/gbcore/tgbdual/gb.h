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

#ifndef GB_H
#define GB_H

#include <stdio.h>
#include <string.h>
#include "gb_types.h"
//#include "../syscall.h"
/* GBAdhoc: MasterBoy's renderer.h/zlib.h replaced by the port layer. */
#include "tgb_port.h"

//Mettre cheat_enable à 1 si des cheats sont appliqués
/* GBAdhoc: cheats are not offered, and CHEAT_SUPPORT costs a 64 KiB
 * cheat_map lookup on every emulated write (cpu_write).  cheat.c is not
 * built.  Define TGB_CHEATS to restore. */
#ifdef TGB_CHEATS
#define CHEAT_SUPPORT
#endif
extern int cheat_enable;


#define INT_VBLANK 1
#define INT_LCDC 2
#define INT_TIMER 4
#define INT_SERIAL 8
#define INT_PAD 16

struct ext_hook{
	byte (*send)(byte);
	char (*led)(void);
};

typedef struct {
	char enable;
	byte code;
	word adr;
	byte dat;
	byte dat_old;
	char name[255];
} cheat_dat;

struct gb_regs {
	byte P1,SB,SC,DIV,TIMA,TMA,TAC,IF,LCDC,STAT,SCY,SCX,LY,LYC,DMA,BGP,OBP1,OBP2,WY,WX,IE;
};

struct gbc_regs {
	byte KEY1,VBK,HDMA1,HDMA2,HDMA3,HDMA4,HDMA5,RP,BCPS,BCPD,OCPS,OCPD,SVBK;
};

union pare_reg {
	word w;
	struct{byte l,h;}b;
};

struct cpu_regs {
	union pare_reg AF;
	union pare_reg BC;
	union pare_reg DE;
	union pare_reg HL;
	word SP;
	word PC;
	byte I;
};


struct apu_stat{
	char sq1_playing;
	int sq1_sw_time;
	int sq1_sw_dir;
	int sq1_sw_shift;

	int sq1_len;
	int sq1_init_len;
	int sq1_type;

	int sq1_vol;
	int sq1_init_vol;
	int sq1_env_dir;
	int sq1_env_speed;

	int sq1_freq;
	int sq1_init_freq;

	int sq1_hold;


	char sq2_playing;

	int sq2_len;
	int sq2_init_len;
	int sq2_type;

	int sq2_vol;
	int sq2_init_vol;
	int sq2_env_dir;
	int sq2_env_speed;

	int sq2_freq;
	int sq2_init_freq;

	int sq2_hold;


	char wav_playing;
	int wav_vol;
	int wav_freq;
	int wav_init_freq;
	int wav_init_len;
	int wav_len;
	int wav_hold;


	char noi_playing;
	int noi_len;
	int noi_init_len;

	int noi_vol;
	int noi_init_vol;
	int noi_env_dir;
	int noi_env_speed;

	int noi_freq;
	int noi_init_freq;
	int noi_hold;
	int noi_step;

	int master_enable;
	int ch_enable[4][2];
	int master_vol[2];
	int ch_on[4];
	int wav_enable;
};

struct apu_que {
	word adr;
	byte dat;
	int clock;
};

struct rom_info {
	char cart_name[18];
	int cart_type;
	byte rom_size;
	byte ram_size;

	char check_sum;
	int gb_type;
};

// gb.c
void gb_init(void);
void gb_reset(void);
void gb_hook_extport(struct ext_hook *ext);
void gb_unhook_extport();
void gb_set_skip(int frame);
char gb_load_rom(byte *buf,int size,byte *ram,int ram_size);
void gb_refresh_pal();
int gb_run();
void gb_fill_vframe(word color);
void gb_invalidate_all_colors();
/* GBAdhoc: states go to/from memory only (MasterBoy's VIRTUAL_FILE path is
 * gone).  gb_save_state(NULL) returns the size without writing. */
size_t gb_save_state(byte *buf);
int gb_restore_state(const byte *buf, size_t size);

extern unsigned char gbc_cram_expand_table[32];
#define MAKE_COLOR15(r, g, b)				MAKE_COLOR(gbc_cram_expand_table[r], gbc_cram_expand_table[g], gbc_cram_expand_table[b])
#define CONVERT_COLOR15(col)				MAKE_COLOR(gbc_cram_expand_table[(col) & 0x1f], gbc_cram_expand_table[((col) >> 5) & 0x1f], gbc_cram_expand_table[((col) >> 10) & 0x1f])

extern word *vframe;
extern struct gb_regs g_regs;
extern struct gbc_regs cg_regs;
extern struct ext_hook hook_proc;
extern char hook_ext;
extern char use_gba;
extern int now_gb_mode;
extern int gbSkip, now_frame;

extern unsigned short sgb_palette_real[8*16];
extern unsigned short sgb_palette_inval[8*16];
extern unsigned char sgb_palette_invalidated;

// lcd.c
//#ifdef USE_GPU
#define SIZE_LINE    256		//in pixel
#define SIZE_LINE_CLIP 176
#define GUARD_LINE   48			//in pixel
//#else
//#define SIZE_LINE    176		//in pixel
//#define GUARD_LINE   8			//in pixel
//#endif

#define VFRAME_SIZE (SIZE_LINE*256*2)

enum{
	PAL_STANDARD,
	PAL_SGB,
};
//extern const char *pal_names[];

void lcd_init(void);
void lcd_set_mpal(int n);
int lcd_get_mpal();
void lcd_render(void *buf,int scanline);
void lcd_reset();
void lcd_clear_win_count();
word *lcd_get_pal(int num);
void gbc_invalidate_color(int palno, int color);
void lcd_set_enable(int layer,char enable);
char lcd_get_enable(int layer);
int lcd_get_sprite_count();
void lcd_bg_render(void *buf,int scanline);
void lcd_win_render(void *buf,int scanline);
void lcd_sprite_render(void *buf,int scanline);
void lcd_bg_render_color(void *buf,int scanline);
void lcd_win_render_color(void *buf,int scanline);
void lcd_sprite_render_color(void *buf,int scanline);

// mbc.c
extern void (*mbc_write)(word adr,byte dat);

void mbc_init();

//byte *mbc_get_rom();
//byte *mbc_get_sram();
//char mbc_is_ext_ram();
//void set_ext_is(char ext);

int mbc_get_state();
void mbc_set_state(int dat);
void mbc_set_page(int arg_rom,int arg_sram);

byte mbc_read(word adr);
void mbc_set_write();
//void mbc_write(word adr,byte dat);
byte mbc_ext_read(word adr);
void mbc_ext_write(word adr,byte dat);
void mbc_reset();

void mbc_nop(word adr,byte dat);
void mbc_mbc1_write(word adr,byte dat);
void mbc_mbc2_write(word adr,byte dat);
void mbc_mbc3_write(word adr,byte dat);
void mbc_mbc5_write(word adr,byte dat);
void mbc_mbc7_write(word adr,byte dat);
void mbc_huc1_write(word adr,byte dat);
void mbc_huc3_write(word adr,byte dat);
void mbc_tama5_write(word adr,byte dat);
void mbc_mmm01_write(word adr,byte dat);

// cpu.c
extern byte vram[];
extern struct cpu_regs c_regs;
extern byte ram[];
extern byte oam[];

extern byte *vram_bank;
extern byte *ram_bank;
extern char halt,speed,speed_change,dma_executing;
extern int dma_src;
extern int dma_dest;
extern int dma_rest;
extern int gdma_rest;
extern char b_dma_first;
extern byte *dma_src_bank;
extern byte *dma_dest_bank;

void cpu_init(void);
void cpu_reset(void);
void cpu_save_state(int *dat);
void cpu_save_state_ex(int *dat);
void cpu_restore_state(int *dat);
void cpu_restore_state_ex(int *dat);
/* GBAdhoc: `inline` dropped from these prototypes (the definitions keep
 * it; the core builds with -fgnu89-inline).  An inline declaration in a
 * header warns in every other translation unit that includes it. */
byte cpu_read_direct(word adr);
byte cpu_read(word adr);
word readw(word adr);
void writew(word adr,word dat);
byte op_read();
word op_readw();
void cpu_write(word adr,byte dat);
void cpu_write_direct(word adr,byte dat);
byte cpu_io_read(word adr);
void cpu_io_write(word adr,byte dat);
void cpu_irq(int irq_type);
void cpu_irq_check();
void cpu_irq_process();
void cpu_exec(unsigned short clocks);

void cpu_set_trace(char trace);
byte *cpu_get_vram();
byte *cpu_get_ram();
byte *cpu_get_oam();
byte *cpu_get_stack();
byte *cpu_get_ram_bank();
void cpu_set_ram_bank(int bank);
struct cpu_regs *cpu_get_regs();
int cpu_get_clock();
char cpu_get_speed();
char *cpu_get_halt();

struct cpu_regs *cpu_get_c_regs();
struct cpu_regs *cpu_set_c_regs();

// rom.c
#define MAX_ROM_SIZE 8*1024*1024


extern int sgb_mode, org_gbtype;

void rom_init();
char rom_has_battery();
int rom_get_sram_size();
char rom_load_rom(byte *buf,int size,byte *ram,int ram_size);
//struct rom_info *rom_get_info();
//byte *get_rom();
//byte *get_sram();
char rom_get_loaded();
void rom_set_first(int page);

// apu.c
void apu_init();
void apu_reset();
byte apu_read(word adr);
void apu_write(word adr,byte dat,int clock);
void apu_update();
struct apu_stat *apu_get_stat_cpu();
struct apu_stat *apu_get_stat_gen();
byte *apu_get_mem();

void snd_init();
void snd_reset();
void snd_set_enable(int ch,char enable);
char snd_get_enable(int ch);

void snd_process(word adr,byte dat);
void snd_update();

short snd_sq1_produce(int freq);
short snd_sq2_produce(int freq);
short snd_wav_produce(int freq);
short snd_noi_produce(int freq);
void snd_render(short *buf1, short *buf2, int sample);
void snd_render_orig(short *buf,int sample);

// cheat.c
extern byte cheat_map[0x10000];
extern cheat_dat cheats[];
extern int nCheats;

void cheat_decreate_cheat_map();
void cheat_create_cheat_map();
int cheat_load(FILE *file);
byte cheat_read(word adr);
void cheat_init();
byte cheat_write(word adr, byte dat);

// sgb.c
extern int bit_received;
extern int bits_received;
extern int packets_received;
extern int sgb_state;
extern int sgb_index;
extern int sgb_multiplayer;
extern int sgb_fourplayers;
extern int sgb_nextcontrol;
extern int sgb_readingcontrol;
extern int sgb_mask;
extern unsigned short sgb_palette[8*16];
extern unsigned short sgb_palette_memory[512*4];
extern byte sgb_buffer[16*7];
extern byte sgb_ATF[18*20];
extern byte sgb_ATF_list[45 * 20 * 18];
extern byte sgb_border[2048];
extern byte sgb_borderchar[32*256];
extern unsigned short sgb_border_buffer[256*224];
extern int border_uploaded;

void sgb_transfer(byte bdata);
void sgb_init();
void sgb_reset_state();
void sgb_reset();


extern struct rom_info info;
extern byte *sram;
extern byte *first_page;
extern byte *rom_page;
extern byte *sram_page;
extern char ext_is_ram;

/*static inline struct rom_info *rom_get_info()
{
	return &info;
}*/
#define rom_get_info()		(&info)

static inline byte *get_rom()
{
	return first_page;
}

static inline byte *get_sram()
{
	return sram;
}


static inline byte *mbc_get_rom()
{
	return rom_page;
}

static inline byte *mbc_get_sram()
{
	return sram_page;
}

static inline char mbc_is_ext_ram()
{
	return ext_is_ram;
}

static inline void mbc_set_ext_is(char ext)
{
	ext_is_ram=ext;
}

extern word c_regs_PC;

#endif
