/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// Not-so-important todo:
// - stm reglist writeback when base is in the list needs adjustment
// - block memory needs psr swapping and user mode reg swapping

#include "common.h"
#include "gpsp_profile.h"   /* named build profiles + illegal-flag rejection */
#if defined(VITA)
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#elif defined(PS2)
#include <kernel.h>
#endif

u8 *last_rom_translation_ptr = NULL;
u8 *last_ram_translation_ptr = NULL;

#if defined(MMAP_JIT_CACHE)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
#elif defined(VITA)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
int sceBlock;
#elif defined(_3DS) 
u8* rom_translation_cache_ptr;
u8* ram_translation_cache_ptr;
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#else
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#endif
/* Note, see stub files for more cache definitions */

u32 iwram_code_min = ~0U;
u32 iwram_code_max =  0U;
u32 ewram_code_min = ~0U;
u32 ewram_code_max =  0U;

#define INITIAL_ROM_WATERMARK   16   // To avoid NULL aliasing
u32 rom_cache_watermark = INITIAL_ROM_WATERMARK;

u8 *bios_swi_entrypoint = NULL;

// Contains an offset table to rom_translation cache area
// It features a chaining linked list for collisions
// The rom area has a small header section that contains:
//  - PC value for the entry
//  - Offset to the next entry (if any)
typedef struct
{
  u32 pc_value;
  u32 next_entry;
} hashhdr_type;

u32 rom_branch_hash[ROM_BRANCH_HASH_SIZE];

typedef struct
{
  u8 *block_offset;
  u16 flag_data;
  u8 condition;
  u8 update_cycles;
} block_data_type;

typedef struct
{
  u32 branch_target;
  u8 *branch_source;
} block_exit_type;

// Div (6) and DivArm (7)
#define is_div_swi(swinum) (((swinum) & 0xFE) == 0x06)

#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_psr_imm(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F                                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F)                       \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF                                                \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF                                              \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)(opcode & 0xFFFFFF) << 8) >> 6                           \

#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF                                                     \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07)                           \

#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F                                                     \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF                                                \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \

/* Include the right emitter headers */
#if defined(MIPS_ARCH)
  #include "mips/mips_emit.h"
#elif defined(ARM_ARCH)
  #include "arm/arm_emit.h"
#elif defined(ARM64_ARCH)
  #include "arm/arm64_emit.h"
#else
  #include "x86/x86_emit.h"
#endif

/* Cache invalidation */

#if defined(PSP)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelDcacheWritebackRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
    sceKernelIcacheInvalidateRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
  }
#elif defined(PS2)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    FlushCache(0);   // Dcache flush
    FlushCache(2);   // Icache invalidate
  }
#elif defined(VITA)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelSyncVMDomain(sceBlock, baseaddr, ((char*)endptr) - ((char*)baseaddr) + 64);
  }
#elif defined(_3DS)
  #include "3ds/3ds_utils.h"
  void platform_cache_sync(void *baseaddr, void *endptr) {
    ctr_flush_invalidate_cache();
  }
#elif defined(ARM_ARCH) || defined(ARM64_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __clear_cache(baseaddr, endptr);
  }
#elif defined(MIPS_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __builtin___clear_cache(baseaddr, endptr);
  }
#else
  /* x86 CPUs have icache consistency checks */
  void platform_cache_sync(void *baseaddr, void *endptr) {}
#endif

void translate_icache_sync() {
    // Cache emitted code can only grow
    if (last_rom_translation_ptr < rom_translation_ptr) {
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
        last_rom_translation_ptr = rom_translation_ptr;
    }
    if (last_ram_translation_ptr < ram_translation_ptr) {
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
}

/* End of Cache invalidation */


#define check_pc_region(pc)                                                   \
  new_pc_region = (pc >> 15);                                                 \
  if(new_pc_region != pc_region)                                              \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    pc_address_block = memory_map_read[new_pc_region];                        \
                                                                              \
    if(!pc_address_block)                                                     \
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);                \
  }                                                                           \

#define translate_arm_instruction()                                           \
  check_pc_region(pc);                                                        \
  opcode = readaddress32(pc_address_block, (pc & 0x7FFF));                    \
  condition = block_data[block_data_position].condition;                      \
                                                                              \
  if((condition != last_condition) || (condition >= 0x20))                    \
  {                                                                           \
    if((last_condition & 0x0F) != 0x0E)                                       \
    {                                                                         \
      generate_branch_patch_conditional(backpatch_address, translation_ptr);  \
    }                                                                         \
                                                                              \
    last_condition = condition;                                               \
                                                                              \
    condition &= 0x0F;                                                        \
                                                                              \
    if(condition != 0x0E)                                                     \
    {                                                                         \
      arm_conditional_block_header();                                         \
    }                                                                         \
  }                                                                           \
  emit_trace_arm_instruction(pc);                                             \
                                                                              \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x00:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MUL rd, rm, rs */                                                \
          arm_multiply(no, no);                                               \
          cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* AND rd, rn, reg_op */                                              \
        arm_data_proc(and, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x01:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MULS rd, rm, rs */                                             \
            arm_multiply(no, yes);                                            \
            cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ANDS rd, rn, reg_op */                                             \
        arm_data_proc(ands, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x02:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MLA rd, rm, rs, rn */                                            \
          arm_multiply(yes, no);                                              \
          cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EOR rd, rn, reg_op */                                              \
        arm_data_proc(eor, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MLAS rd, rm, rs, rn */                                         \
            arm_multiply(yes, yes);                                           \
            cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EORS rd, rn, reg_op */                                             \
        arm_data_proc(eors, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUB rd, rn, reg_op */                                              \
        arm_data_proc(sub, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUBS rd, rn, reg_op */                                             \
        arm_data_proc(subs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSB rd, rn, reg_op */                                              \
        arm_data_proc(rsb, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSBS rd, rn, reg_op */                                             \
        arm_data_proc(rsbs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMULL rd, rm, rs */                                              \
          arm_multiply_long(u64, no, no);                                     \
          cycle_count += 3;  /* this is an aproximation :P */                 \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD rd, rn, reg_op */                                              \
        arm_data_proc(add, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x09:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64, no, yes);                                  \
            cycle_count += 3;  /* this is an aproximation :P */               \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADDS rd, rn, reg_op */                                             \
        arm_data_proc(adds, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMLAL rd, rm, rs */                                              \
          arm_multiply_long(u64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADC rd, rn, reg_op */                                              \
        arm_data_proc(adc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADCS rd, rn, reg_op */                                             \
        arm_data_proc(adcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMULL rd, rm, rs */                                              \
          arm_multiply_long(s64, no, no);                                     \
          cycle_count += 2;  /* Between 1 and 4 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBC rd, rn, reg_op */                                              \
        arm_data_proc(sbc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64, no, yes);                                  \
            cycle_count += 2;  /* Between 1 and 4 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBCS rd, rn, reg_op */                                             \
        arm_data_proc(sbcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMLAL rd, rm, rs */                                              \
          arm_multiply_long(s64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSC rd, rn, reg_op */                                              \
        arm_data_proc(rsc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSCS rd, rn, reg_op */                                             \
        arm_data_proc(rscs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x10:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - rm] */                                            \
          arm_access_memory(store, down, pre, u16, half_reg);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWP rd, rm, [rn] */                                              \
          arm_swap(u32);                                                      \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, cpsr */                                                    \
        arm_psr(reg, read, cpsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x11:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm] */                                          \
            arm_access_memory(load, down, pre, u16, half_reg);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s8, half_reg);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s16, half_reg);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TST rd, rn, reg_op */                                              \
        arm_data_proc_test(tst, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - rm]! */                                             \
        arm_access_memory(store, down, pre_wb, u16, half_reg);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(opcode & 0x10)                                                     \
        {                                                                     \
          /* BX rn */                                                         \
          arm_bx();                                                           \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MSR cpsr, rm */                                                  \
          arm_psr(reg, store, cpsr);                                          \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x13:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm]! */                                         \
            arm_access_memory(load, down, pre_wb, u16, half_reg);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s8, half_reg);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s16, half_reg);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TEQ rd, rn, reg_op */                                              \
        arm_data_proc_test(teq, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x14:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - imm] */                                           \
          arm_access_memory(store, down, pre, u16, half_imm);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWPB rd, rm, [rn] */                                             \
          arm_swap(u8);                                                       \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, spsr */                                                    \
        arm_psr(reg, read, spsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x15:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm] */                                         \
            arm_access_memory(load, down, pre, u16, half_imm);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s8, half_imm);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s16, half_imm);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMP rn, reg_op */                                                  \
        arm_data_proc_test(cmp, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x16:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - imm]! */                                            \
        arm_access_memory(store, down, pre_wb, u16, half_imm);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MSR spsr, rm */                                                    \
        arm_psr(reg, store, spsr);                                            \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x17:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm]! */                                        \
            arm_access_memory(load, down, pre_wb, u16, half_imm);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s8, half_imm);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s16, half_imm);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMN rd, rn, reg_op */                                              \
        arm_data_proc_test(cmn, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x18:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm] */                                              \
        arm_access_memory(store, up, pre, u16, half_reg);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORR rd, rn, reg_op */                                              \
        arm_data_proc(orr, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x19:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm] */                                          \
            arm_access_memory(load, up, pre, u16, half_reg);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s8, half_reg);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s16, half_reg);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORRS rd, rn, reg_op */                                             \
        arm_data_proc(orrs, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm]! */                                             \
        arm_access_memory(store, up, pre_wb, u16, half_reg);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOV rd, reg_op */                                                  \
        arm_data_proc_unary(mov, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm]! */                                         \
            arm_access_memory(load, up, pre_wb, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOVS rd, reg_op */                                                 \
        arm_data_proc_unary(movs, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm] */                                             \
        arm_access_memory(store, up, pre, u16, half_imm);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BIC rd, rn, reg_op */                                              \
        arm_data_proc(bic, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm] */                                         \
            arm_access_memory(load, up, pre, u16, half_imm);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s8, half_imm);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s16, half_imm);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BICS rd, rn, reg_op */                                             \
        arm_data_proc(bics, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm]! */                                            \
        arm_access_memory(store, up, pre_wb, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVN rd, reg_op */                                                  \
        arm_data_proc_unary(mvn, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm]! */                                        \
            arm_access_memory(load, up, pre_wb, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVNS rd, rn, reg_op */                                             \
        arm_data_proc_unary(mvns, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x20:                                                                \
      /* AND rd, rn, imm */                                                   \
      arm_data_proc(and, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
      /* ANDS rd, rn, imm */                                                  \
      arm_data_proc(ands, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x22:                                                                \
      /* EOR rd, rn, imm */                                                   \
      arm_data_proc(eor, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x23:                                                                \
      /* EORS rd, rn, imm */                                                  \
      arm_data_proc(eors, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x24:                                                                \
      /* SUB rd, rn, imm */                                                   \
      arm_data_proc(sub, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x25:                                                                \
      /* SUBS rd, rn, imm */                                                  \
      arm_data_proc(subs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x26:                                                                \
      /* RSB rd, rn, imm */                                                   \
      arm_data_proc(rsb, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x27:                                                                \
      /* RSBS rd, rn, imm */                                                  \
      arm_data_proc(rsbs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x28:                                                                \
      /* ADD rd, rn, imm */                                                   \
      arm_data_proc(add, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x29:                                                                \
      /* ADDS rd, rn, imm */                                                  \
      arm_data_proc(adds, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2A:                                                                \
      /* ADC rd, rn, imm */                                                   \
      arm_data_proc(adc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2B:                                                                \
      /* ADCS rd, rn, imm */                                                  \
      arm_data_proc(adcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2C:                                                                \
      /* SBC rd, rn, imm */                                                   \
      arm_data_proc(sbc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2D:                                                                \
      /* SBCS rd, rn, imm */                                                  \
      arm_data_proc(sbcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2E:                                                                \
      /* RSC rd, rn, imm */                                                   \
      arm_data_proc(rsc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2F:                                                                \
      /* RSCS rd, rn, imm */                                                  \
      arm_data_proc(rscs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x30 ... 0x31:                                                       \
      /* TST rn, imm */                                                       \
      arm_data_proc_test(tst, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x32:                                                                \
      /* MSR cpsr, imm */                                                     \
      arm_psr(imm, store, cpsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x33:                                                                \
      /* TEQ rn, imm */                                                       \
      arm_data_proc_test(teq, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x34 ... 0x35:                                                       \
      /* CMP rn, imm */                                                       \
      arm_data_proc_test(cmp, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x36:                                                                \
      /* MSR spsr, imm */                                                     \
      arm_psr(imm, store, spsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x37:                                                                \
      /* CMN rn, imm */                                                       \
      arm_data_proc_test(cmn, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x38:                                                                \
      /* ORR rd, rn, imm */                                                   \
      arm_data_proc(orr, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x39:                                                                \
      /* ORRS rd, rn, imm */                                                  \
      arm_data_proc(orrs, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3A:                                                                \
      /* MOV rd, imm */                                                       \
      arm_data_proc_unary(mov, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3B:                                                                \
      /* MOVS rd, imm */                                                      \
      arm_data_proc_unary(movs, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x3C:                                                                \
      /* BIC rd, rn, imm */                                                   \
      arm_data_proc(bic, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x3D:                                                                \
      /* BICS rd, rn, imm */                                                  \
      arm_data_proc(bics, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3E:                                                                \
      /* MVN rd, imm */                                                       \
      arm_data_proc_unary(mvn, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3F:                                                                \
      /* MVNS rd, imm */                                                      \
      arm_data_proc_unary(mvns, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      /* STR rd, [rn], -imm */                                                \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      /* LDR rd, [rn], -imm */                                                \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      /* STRT rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      /* LDRT rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* STRB rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* LDRB rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* STRBT rd, [rn], -imm */                                              \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* LDRBT rd, [rn], -imm */                                              \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x48:                                                                \
      /* STR rd, [rn], +imm */                                                \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x49:                                                                \
      /* LDR rd, [rn], +imm */                                                \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4A:                                                                \
      /* STRT rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x4B:                                                                \
      /* LDRT rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4C:                                                                \
      /* STRB rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4D:                                                                \
      /* LDRB rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x4E:                                                                \
      /* STRBT rd, [rn], +imm */                                              \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4F:                                                                \
      /* LDRBT rd, [rn], +imm */                                              \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x50:                                                                \
      /* STR rd, [rn - imm] */                                                \
      arm_access_memory(store, down, pre, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x51:                                                                \
      /* LDR rd, [rn - imm] */                                                \
      arm_access_memory(load, down, pre, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x52:                                                                \
      /* STR rd, [rn - imm]! */                                               \
      arm_access_memory(store, down, pre_wb, u32, imm);                       \
      break;                                                                  \
                                                                              \
    case 0x53:                                                                \
      /* LDR rd, [rn - imm]! */                                               \
      arm_access_memory(load, down, pre_wb, u32, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x54:                                                                \
      /* STRB rd, [rn - imm] */                                               \
      arm_access_memory(store, down, pre, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x55:                                                                \
      /* LDRB rd, [rn - imm] */                                               \
      arm_access_memory(load, down, pre, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x56:                                                                \
      /* STRB rd, [rn - imm]! */                                              \
      arm_access_memory(store, down, pre_wb, u8, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x57:                                                                \
      /* LDRB rd, [rn - imm]! */                                              \
      arm_access_memory(load, down, pre_wb, u8, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x58:                                                                \
      /* STR rd, [rn + imm] */                                                \
      arm_access_memory(store, up, pre, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x59:                                                                \
      /* LDR rd, [rn + imm] */                                                \
      arm_access_memory(load, up, pre, u32, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5A:                                                                \
      /* STR rd, [rn + imm]! */                                               \
      arm_access_memory(store, up, pre_wb, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x5B:                                                                \
      /* LDR rd, [rn + imm]! */                                               \
      arm_access_memory(load, up, pre_wb, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5C:                                                                \
      /* STRB rd, [rn + imm] */                                               \
      arm_access_memory(store, up, pre, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5D:                                                                \
      /* LDRB rd, [rn + imm] */                                               \
      arm_access_memory(load, up, pre, u8, imm);                              \
      break;                                                                  \
                                                                              \
    case 0x5E:                                                                \
      /* STRB rd, [rn + imm]! */                                              \
      arm_access_memory(store, up, pre_wb, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5F:                                                                \
      /* LDRBT rd, [rn + imm]! */                                             \
      arm_access_memory(load, up, pre_wb, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x60:                                                                \
      /* STR rd, [rn], -rm */                                                 \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x61:                                                                \
      /* LDR rd, [rn], -rm */                                                 \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x62:                                                                \
      /* STRT rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x63:                                                                \
      /* LDRT rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x64:                                                                \
      /* STRB rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x65:                                                                \
      /* LDRB rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x66:                                                                \
      /* STRBT rd, [rn], -rm */                                               \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x67:                                                                \
      /* LDRBT rd, [rn], -rm */                                               \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x68:                                                                \
      /* STR rd, [rn], +rm */                                                 \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x69:                                                                \
      /* LDR rd, [rn], +rm */                                                 \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6A:                                                                \
      /* STRT rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x6B:                                                                \
      /* LDRT rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6C:                                                                \
      /* STRB rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6D:                                                                \
      /* LDRB rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x6E:                                                                \
      /* STRBT rd, [rn], +rm */                                               \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6F:                                                                \
      /* LDRBT rd, [rn], +rm */                                               \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x70:                                                                \
      /* STR rd, [rn - rm] */                                                 \
      arm_access_memory(store, down, pre, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x71:                                                                \
      /* LDR rd, [rn - rm] */                                                 \
      arm_access_memory(load, down, pre, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x72:                                                                \
      /* STR rd, [rn - rm]! */                                                \
      arm_access_memory(store, down, pre_wb, u32, reg);                       \
      break;                                                                  \
                                                                              \
    case 0x73:                                                                \
      /* LDR rd, [rn - rm]! */                                                \
      arm_access_memory(load, down, pre_wb, u32, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x74:                                                                \
      /* STRB rd, [rn - rm] */                                                \
      arm_access_memory(store, down, pre, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x75:                                                                \
      /* LDRB rd, [rn - rm] */                                                \
      arm_access_memory(load, down, pre, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x76:                                                                \
      /* STRB rd, [rn - rm]! */                                               \
      arm_access_memory(store, down, pre_wb, u8, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x77:                                                                \
      /* LDRB rd, [rn - rm]! */                                               \
      arm_access_memory(load, down, pre_wb, u8, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x78:                                                                \
      /* STR rd, [rn + rm] */                                                 \
      arm_access_memory(store, up, pre, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x79:                                                                \
      /* LDR rd, [rn + rm] */                                                 \
      arm_access_memory(load, up, pre, u32, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7A:                                                                \
      /* STR rd, [rn + rm]! */                                                \
      arm_access_memory(store, up, pre_wb, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x7B:                                                                \
      /* LDR rd, [rn + rm]! */                                                \
      arm_access_memory(load, up, pre_wb, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7C:                                                                \
      /* STRB rd, [rn + rm] */                                                \
      arm_access_memory(store, up, pre, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7D:                                                                \
      /* LDRB rd, [rn + rm] */                                                \
      arm_access_memory(load, up, pre, u8, reg);                              \
      break;                                                                  \
                                                                              \
    case 0x7E:                                                                \
      /* STRB rd, [rn + rm]! */                                               \
      arm_access_memory(store, up, pre_wb, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7F:                                                                \
      /* LDRBT rd, [rn + rm]! */                                              \
      arm_access_memory(load, up, pre_wb, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x80:                                                                \
      /* STMDA rn, rlist */                                                   \
      arm_block_memory(store, down_a, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x81:                                                                \
      /* LDMDA rn, rlist */                                                   \
      arm_block_memory(load, down_a, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x82:                                                                \
      /* STMDA rn!, rlist */                                                  \
      arm_block_memory(store, down_a, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x83:                                                                \
      /* LDMDA rn!, rlist */                                                  \
      arm_block_memory(load, down_a, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x84:                                                                \
      /* STMDA rn, rlist^ */                                                  \
      arm_block_memory(store, down_a, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x85:                                                                \
      /* LDMDA rn, rlist^ */                                                  \
      arm_block_memory(load, down_a, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x86:                                                                \
      /* STMDA rn!, rlist^ */                                                 \
      arm_block_memory(store, down_a, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x87:                                                                \
      /* LDMDA rn!, rlist^ */                                                 \
      arm_block_memory(load, down_a, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x88:                                                                \
      /* STMIA rn, rlist */                                                   \
      arm_block_memory(store, no, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x89:                                                                \
      /* LDMIA rn, rlist */                                                   \
      arm_block_memory(load, no, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8A:                                                                \
      /* STMIA rn!, rlist */                                                  \
      arm_block_memory(store, no, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x8B:                                                                \
      /* LDMIA rn!, rlist */                                                  \
      arm_block_memory(load, no, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8C:                                                                \
      /* STMIA rn, rlist^ */                                                  \
      arm_block_memory(store, no, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8D:                                                                \
      /* LDMIA rn, rlist^ */                                                  \
      arm_block_memory(load, no, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x8E:                                                                \
      /* STMIA rn!, rlist^ */                                                 \
      arm_block_memory(store, no, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8F:                                                                \
      /* LDMIA rn!, rlist^ */                                                 \
      arm_block_memory(load, no, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x90:                                                                \
      /* STMDB rn, rlist */                                                   \
      arm_block_memory(store, down_b, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x91:                                                                \
      /* LDMDB rn, rlist */                                                   \
      arm_block_memory(load, down_b, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x92:                                                                \
      /* STMDB rn!, rlist */                                                  \
      arm_block_memory(store, down_b, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x93:                                                                \
      /* LDMDB rn!, rlist */                                                  \
      arm_block_memory(load, down_b, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x94:                                                                \
      /* STMDB rn, rlist^ */                                                  \
      arm_block_memory(store, down_b, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x95:                                                                \
      /* LDMDB rn, rlist^ */                                                  \
      arm_block_memory(load, down_b, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x96:                                                                \
      /* STMDB rn!, rlist^ */                                                 \
      arm_block_memory(store, down_b, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x97:                                                                \
      /* LDMDB rn!, rlist^ */                                                 \
      arm_block_memory(load, down_b, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x98:                                                                \
      /* STMIB rn, rlist */                                                   \
      arm_block_memory(store, up, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x99:                                                                \
      /* LDMIB rn, rlist */                                                   \
      arm_block_memory(load, up, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9A:                                                                \
      /* STMIB rn!, rlist */                                                  \
      arm_block_memory(store, up, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x9B:                                                                \
      /* LDMIB rn!, rlist */                                                  \
      arm_block_memory(load, up, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9C:                                                                \
      /* STMIB rn, rlist^ */                                                  \
      arm_block_memory(store, up, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9D:                                                                \
      /* LDMIB rn, rlist^ */                                                  \
      arm_block_memory(load, up, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x9E:                                                                \
      /* STMIB rn!, rlist^ */                                                 \
      arm_block_memory(store, up, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9F:                                                                \
      /* LDMIB rn!, rlist^ */                                                 \
      arm_block_memory(load, up, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0xA0 ... 0xAF:                                                       \
    {                                                                         \
      /* B offset */                                                          \
      arm_b();                                                                \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xB0 ... 0xBF:                                                       \
    {                                                                         \
      /* BL offset */                                                         \
      arm_bl();                                                               \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xFF:                                                       \
    {                                                                         \
      u32 swinum = (opcode >> 16) & 0xFF;                                     \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(arm);                                                     \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(arm);                                                 \
      }                                                                       \
      else {                                                                  \
        arm_swi();                                                            \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 4                                                                     \

#define arm_flag_status()                                                     \

#define translate_thumb_instruction()                                         \
  flag_status = block_data[block_data_position].flag_data;                    \
  check_pc_region(pc);                                                        \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (pc & 0x7FFF));                    \
  emit_trace_thumb_instruction(pc);                                           \
  u8 hiop = opcode >> 8;                                                      \
                                                                              \
  switch(hiop)                                                                \
  {                                                                           \
    case 0x00 ... 0x07:                                                       \
      /* LSL rd, rs, imm */                                                   \
      thumb_shift(shift, lsl, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x08 ... 0x0F:                                                       \
      /* LSR rd, rs, imm */                                                   \
      thumb_shift(shift, lsr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x10 ... 0x17:                                                       \
      /* ASR rd, rs, imm */                                                   \
      thumb_shift(shift, asr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x18 ... 0x19:                                                       \
      /* ADD rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, adds, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1A ... 0x1B:                                                       \
      /* SUB rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, subs, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1C ... 0x1D:                                                       \
      /* ADD rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, adds, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    case 0x1E ... 0x1F:                                                       \
      /* SUB rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, subs, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    /* MOV r0..7, imm */                                                      \
    case 0x20: thumb_data_proc_unary(imm, movs, imm, 0, imm); break;          \
    case 0x21: thumb_data_proc_unary(imm, movs, imm, 1, imm); break;          \
    case 0x22: thumb_data_proc_unary(imm, movs, imm, 2, imm); break;          \
    case 0x23: thumb_data_proc_unary(imm, movs, imm, 3, imm); break;          \
    case 0x24: thumb_data_proc_unary(imm, movs, imm, 4, imm); break;          \
    case 0x25: thumb_data_proc_unary(imm, movs, imm, 5, imm); break;          \
    case 0x26: thumb_data_proc_unary(imm, movs, imm, 6, imm); break;          \
    case 0x27: thumb_data_proc_unary(imm, movs, imm, 7, imm); break;          \
                                                                              \
    /* CMP r0, imm */                                                         \
    case 0x28: thumb_data_proc_test(imm, cmp, imm, 0, imm); break;            \
    case 0x29: thumb_data_proc_test(imm, cmp, imm, 1, imm); break;            \
    case 0x2A: thumb_data_proc_test(imm, cmp, imm, 2, imm); break;            \
    case 0x2B: thumb_data_proc_test(imm, cmp, imm, 3, imm); break;            \
    case 0x2C: thumb_data_proc_test(imm, cmp, imm, 4, imm); break;            \
    case 0x2D: thumb_data_proc_test(imm, cmp, imm, 5, imm); break;            \
    case 0x2E: thumb_data_proc_test(imm, cmp, imm, 6, imm); break;            \
    case 0x2F: thumb_data_proc_test(imm, cmp, imm, 7, imm); break;            \
                                                                              \
    /* ADD r0..7, imm */                                                      \
    case 0x30: thumb_data_proc(imm, adds, imm, 0, 0, imm); break;             \
    case 0x31: thumb_data_proc(imm, adds, imm, 1, 1, imm); break;             \
    case 0x32: thumb_data_proc(imm, adds, imm, 2, 2, imm); break;             \
    case 0x33: thumb_data_proc(imm, adds, imm, 3, 3, imm); break;             \
    case 0x34: thumb_data_proc(imm, adds, imm, 4, 4, imm); break;             \
    case 0x35: thumb_data_proc(imm, adds, imm, 5, 5, imm); break;             \
    case 0x36: thumb_data_proc(imm, adds, imm, 6, 6, imm); break;             \
    case 0x37: thumb_data_proc(imm, adds, imm, 7, 7, imm); break;             \
                                                                              \
    /* SUB r0..7, imm */                                                      \
    case 0x38: thumb_data_proc(imm, subs, imm, 0, 0, imm); break;             \
    case 0x39: thumb_data_proc(imm, subs, imm, 1, 1, imm); break;             \
    case 0x3A: thumb_data_proc(imm, subs, imm, 2, 2, imm); break;             \
    case 0x3B: thumb_data_proc(imm, subs, imm, 3, 3, imm); break;             \
    case 0x3C: thumb_data_proc(imm, subs, imm, 4, 4, imm); break;             \
    case 0x3D: thumb_data_proc(imm, subs, imm, 5, 5, imm); break;             \
    case 0x3E: thumb_data_proc(imm, subs, imm, 6, 6, imm); break;             \
    case 0x3F: thumb_data_proc(imm, subs, imm, 7, 7, imm); break;             \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_data_proc(alu_op, ands, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_data_proc(alu_op, eors, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_shift(alu_op, lsl, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_shift(alu_op, lsr, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_shift(alu_op, asr, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_data_proc(alu_op, adcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_data_proc(alu_op, sbcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_shift(alu_op, ror, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* TST rd, rs */                                                    \
          thumb_data_proc_test(alu_op, tst, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* NEG rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, neg, reg, rd, rs);                    \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* CMP rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmp, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* CMN rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmn, reg, rd, rs);                     \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ORR rd, rs */                                                    \
          thumb_data_proc(alu_op, orrs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* MUL rd, rs */                                                    \
          thumb_data_proc(alu_op, muls, reg, rd, rs, rd);                     \
          cycle_count += 2;  /* Between 1 and 4 extra cycles */               \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* BIC rd, rs */                                                    \
          thumb_data_proc(alu_op, bics, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* MVN rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, mvns, reg, rd, rs);                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* ADD rd, rs */                                                        \
      thumb_data_proc_hi(add);                                                \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_data_proc_test_hi(cmp);                                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* MOV rd, rs */                                                        \
      thumb_data_proc_mov_hi();                                               \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* BX rs */                                                             \
      thumb_bx();                                                             \
      break;                                                                  \
                                                                              \
    case 0x48 ... 0x4F:                                                       \
      /* LDR r0..7, [pc + imm] */                                             \
      {                                                                       \
        thumb_decode_imm();                                                   \
        u32 rdreg = (hiop & 7);                                               \
        u32 aoff = (pc & ~2) + (imm*4) + 4;                                   \
        /* ROM + same page -> optimize as const load */                       \
        if (!ram_region && (((aoff + 4) >> 15) == (pc >> 15))) {              \
          u32 value = readaddress32(pc_address_block, (aoff & 0x7FFF));       \
          thumb_load_pc_pool_const(rdreg, value);                             \
        } else {                                                              \
          thumb_access_memory(load, imm, rdreg, 0, 0, pc_relative, aoff, u32);\
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x50 ... 0x51:                                                       \
      /* STR rd, [rb + ro] */                                                 \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u32);       \
      break;                                                                  \
                                                                              \
    case 0x52 ... 0x53:                                                       \
      /* STRH rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u16);       \
      break;                                                                  \
                                                                              \
    case 0x54 ... 0x55:                                                       \
      /* STRB rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u8);        \
      break;                                                                  \
                                                                              \
    case 0x56 ... 0x57:                                                       \
      /* LDSB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s8);         \
      break;                                                                  \
                                                                              \
    case 0x58 ... 0x59:                                                       \
      /* LDR rd, [rb + ro] */                                                 \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u32);        \
      break;                                                                  \
                                                                              \
    case 0x5A ... 0x5B:                                                       \
      /* LDRH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u16);        \
      break;                                                                  \
                                                                              \
    case 0x5C ... 0x5D:                                                       \
      /* LDRB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u8);         \
      break;                                                                  \
                                                                              \
    case 0x5E ... 0x5F:                                                       \
      /* LDSH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s16);        \
      break;                                                                  \
                                                                              \
    case 0x60 ... 0x67:                                                       \
      /* STR rd, [rb + imm] */                                                \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, (imm * 4),      \
       u32);                                                                  \
      break;                                                                  \
                                                                              \
    case 0x68 ... 0x6F:                                                       \
      /* LDR rd, [rb + imm] */                                                \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 4), u32); \
      break;                                                                  \
                                                                              \
    case 0x70 ... 0x77:                                                       \
      /* STRB rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, imm, u8);       \
      break;                                                                  \
                                                                              \
    case 0x78 ... 0x7F:                                                       \
      /* LDRB rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, imm, u8);        \
      break;                                                                  \
                                                                              \
    case 0x80 ... 0x87:                                                       \
      /* STRH rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm,                 \
       (imm * 2), u16);                                                       \
      break;                                                                  \
                                                                              \
    case 0x88 ... 0x8F:                                                       \
      /* LDRH rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 2), u16); \
      break;                                                                  \
                                                                              \
    /* STR r0..7, [sp + imm] */                                               \
    case 0x90:                                                                \
      thumb_access_memory(store, imm, 0, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x91:                                                                \
      thumb_access_memory(store, imm, 1, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x92:                                                                \
      thumb_access_memory(store, imm, 2, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x93:                                                                \
      thumb_access_memory(store, imm, 3, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x94:                                                                \
      thumb_access_memory(store, imm, 4, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x95:                                                                \
      thumb_access_memory(store, imm, 5, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x96:                                                                \
      thumb_access_memory(store, imm, 6, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x97:                                                                \
      thumb_access_memory(store, imm, 7, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
                                                                              \
    /* LDR r0..7, [sp + imm] */                                               \
    case 0x98:                                                                \
      thumb_access_memory(load, imm, 0, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x99:                                                                \
      thumb_access_memory(load, imm, 1, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9A:                                                                \
      thumb_access_memory(load, imm, 2, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9B:                                                                \
      thumb_access_memory(load, imm, 3, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9C:                                                                \
      thumb_access_memory(load, imm, 4, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9D:                                                                \
      thumb_access_memory(load, imm, 5, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9E:                                                                \
      thumb_access_memory(load, imm, 6, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9F:                                                                \
      thumb_access_memory(load, imm, 7, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
                                                                              \
    /* ADD r0..7, pc, +imm */                                                 \
    case 0xA0: thumb_load_pc(0); break;                                       \
    case 0xA1: thumb_load_pc(1); break;                                       \
    case 0xA2: thumb_load_pc(2); break;                                       \
    case 0xA3: thumb_load_pc(3); break;                                       \
    case 0xA4: thumb_load_pc(4); break;                                       \
    case 0xA5: thumb_load_pc(5); break;                                       \
    case 0xA6: thumb_load_pc(6); break;                                       \
    case 0xA7: thumb_load_pc(7); break;                                       \
                                                                              \
    /* ADD r0..7, sp, +imm */                                                 \
    case 0xA8: thumb_load_sp(0); break;                                       \
    case 0xA9: thumb_load_sp(1); break;                                       \
    case 0xAA: thumb_load_sp(2); break;                                       \
    case 0xAB: thumb_load_sp(3); break;                                       \
    case 0xAC: thumb_load_sp(4); break;                                       \
    case 0xAD: thumb_load_sp(5); break;                                       \
    case 0xAE: thumb_load_sp(6); break;                                       \
    case 0xAF: thumb_load_sp(7); break;                                       \
                                                                              \
    case 0xB0 ... 0xB3:                                                       \
      if((opcode >> 7) & 0x01)                                                \
      {                                                                       \
        /* ADD sp, -imm */                                                    \
        thumb_adjust_sp(down);                                                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD sp, +imm */                                                    \
        thumb_adjust_sp(up);                                                  \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0xB4:                                                                \
      /* PUSH rlist */                                                        \
      thumb_block_memory(store, down, no, 13);                                \
      break;                                                                  \
                                                                              \
    case 0xB5:                                                                \
      /* PUSH rlist, lr */                                                    \
      thumb_block_memory(store, push_lr, push_lr, 13);                        \
      break;                                                                  \
                                                                              \
    case 0xBC:                                                                \
      /* POP rlist */                                                         \
      thumb_block_memory(load, no, up, 13);                                   \
      break;                                                                  \
                                                                              \
    case 0xBD:                                                                \
      /* POP rlist, pc */                                                     \
      thumb_block_memory(load, no, pop_pc, 13);                               \
      break;                                                                  \
                                                                              \
    case 0xC0:                                                                \
      /* STMIA r0!, rlist */                                                  \
      thumb_block_memory(store, no, up, 0);                                   \
      break;                                                                  \
                                                                              \
    case 0xC1:                                                                \
      /* STMIA r1!, rlist */                                                  \
      thumb_block_memory(store, no, up, 1);                                   \
      break;                                                                  \
                                                                              \
    case 0xC2:                                                                \
      /* STMIA r2!, rlist */                                                  \
      thumb_block_memory(store, no, up, 2);                                   \
      break;                                                                  \
                                                                              \
    case 0xC3:                                                                \
      /* STMIA r3!, rlist */                                                  \
      thumb_block_memory(store, no, up, 3);                                   \
      break;                                                                  \
                                                                              \
    case 0xC4:                                                                \
      /* STMIA r4!, rlist */                                                  \
      thumb_block_memory(store, no, up, 4);                                   \
      break;                                                                  \
                                                                              \
    case 0xC5:                                                                \
      /* STMIA r5!, rlist */                                                  \
      thumb_block_memory(store, no, up, 5);                                   \
      break;                                                                  \
                                                                              \
    case 0xC6:                                                                \
      /* STMIA r6!, rlist */                                                  \
      thumb_block_memory(store, no, up, 6);                                   \
      break;                                                                  \
                                                                              \
    case 0xC7:                                                                \
      /* STMIA r7!, rlist */                                                  \
      thumb_block_memory(store, no, up, 7);                                   \
      break;                                                                  \
                                                                              \
    case 0xC8:                                                                \
      /* LDMIA r0!, rlist */                                                  \
      thumb_block_memory(load, no, up, 0);                                    \
      break;                                                                  \
                                                                              \
    case 0xC9:                                                                \
      /* LDMIA r1!, rlist */                                                  \
      thumb_block_memory(load, no, up, 1);                                    \
      break;                                                                  \
                                                                              \
    case 0xCA:                                                                \
      /* LDMIA r2!, rlist */                                                  \
      thumb_block_memory(load, no, up, 2);                                    \
      break;                                                                  \
                                                                              \
    case 0xCB:                                                                \
      /* LDMIA r3!, rlist */                                                  \
      thumb_block_memory(load, no, up, 3);                                    \
      break;                                                                  \
                                                                              \
    case 0xCC:                                                                \
      /* LDMIA r4!, rlist */                                                  \
      thumb_block_memory(load, no, up, 4);                                    \
      break;                                                                  \
                                                                              \
    case 0xCD:                                                                \
      /* LDMIA r5!, rlist */                                                  \
      thumb_block_memory(load, no, up, 5);                                    \
      break;                                                                  \
                                                                              \
    case 0xCE:                                                                \
      /* LDMIA r6!, rlist */                                                  \
      thumb_block_memory(load, no, up, 6);                                    \
      break;                                                                  \
                                                                              \
    case 0xCF:                                                                \
      /* LDMIA r7!, rlist */                                                  \
      thumb_block_memory(load, no, up, 7);                                    \
      break;                                                                  \
                                                                              \
    case 0xD0:                                                                \
      /* BEQ label */                                                         \
      thumb_conditional_branch(eq);                                           \
      break;                                                                  \
                                                                              \
    case 0xD1:                                                                \
      /* BNE label */                                                         \
      thumb_conditional_branch(ne);                                           \
      break;                                                                  \
                                                                              \
    case 0xD2:                                                                \
      /* BCS label */                                                         \
      thumb_conditional_branch(cs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD3:                                                                \
      /* BCC label */                                                         \
      thumb_conditional_branch(cc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD4:                                                                \
      /* BMI label */                                                         \
      thumb_conditional_branch(mi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD5:                                                                \
      /* BPL label */                                                         \
      thumb_conditional_branch(pl);                                           \
      break;                                                                  \
                                                                              \
    case 0xD6:                                                                \
      /* BVS label */                                                         \
      thumb_conditional_branch(vs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD7:                                                                \
      /* BVC label */                                                         \
      thumb_conditional_branch(vc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD8:                                                                \
      /* BHI label */                                                         \
      thumb_conditional_branch(hi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD9:                                                                \
      /* BLS label */                                                         \
      thumb_conditional_branch(ls);                                           \
      break;                                                                  \
                                                                              \
    case 0xDA:                                                                \
      /* BGE label */                                                         \
      thumb_conditional_branch(ge);                                           \
      break;                                                                  \
                                                                              \
    case 0xDB:                                                                \
      /* BLT label */                                                         \
      thumb_conditional_branch(lt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDC:                                                                \
      /* BGT label */                                                         \
      thumb_conditional_branch(gt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDD:                                                                \
      /* BLE label */                                                         \
      thumb_conditional_branch(le);                                           \
      break;                                                                  \
                                                                              \
    case 0xDF:                                                                \
    {                                                                         \
      u32 swinum = opcode & 0xFF;                                             \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(thumb);                                                   \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(thumb);                                               \
      }                                                                       \
      else {                                                                  \
        thumb_swi();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xE0 ... 0xE7:                                                       \
    {                                                                         \
      /* B label */                                                           \
      thumb_b();                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xF7:                                                       \
    {                                                                         \
      /* (low word) BL label */                                               \
      /* This should possibly generate code if not in conjunction with a BLH  \
         next, but I don't think anyone will do that. */                      \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF8 ... 0xFF:                                                       \
    {                                                                         \
      /* (high word) BL label */                                              \
      /* This might not be preceeding a BL low word (Golden Sun 2), if so     \
         it must be handled like an indirect branch. */                       \
      if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                   \
      {                                                                       \
        thumb_bl();                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_blh();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 2                                                                     \

#define thumb_flag_modifies_all()                                             \
  flag_status |= 0xFF                                                         \

#define thumb_flag_modifies_zn()                                              \
  flag_status |= 0xCC                                                         \

#define thumb_flag_modifies_znc()                                             \
  flag_status |= 0xEE                                                         \

#define thumb_flag_modifies_zn_maybe_c()                                      \
  flag_status |= 0xCE                                                         \

#define thumb_flag_modifies_c()                                               \
  flag_status |= 0x22                                                         \

#define thumb_flag_requires_c()                                               \
  flag_status |= 0x200                                                        \

#define thumb_flag_requires_all()                                             \
  flag_status |= 0xF00                                                        \

#define thumb_flag_status()                                                   \
{                                                                             \
  u16 flag_status = 0;                                                        \
  switch((opcode >> 8) & 0xFF)                                                \
  {                                                                           \
    /* left shift by imm */                                                   \
    case 0x00 ... 0x07:                                                       \
      thumb_flag_modifies_zn();                                               \
      if(((opcode >> 6) & 0x1F) != 0)                                         \
      {                                                                       \
        thumb_flag_modifies_c();                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* right shift by imm */                                                  \
    case 0x08 ... 0x17:                                                       \
      thumb_flag_modifies_znc();                                              \
      break;                                                                  \
                                                                              \
    /* add, subtract */                                                       \
    case 0x18 ... 0x1F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov reg, imm */                                                        \
    case 0x20 ... 0x27:                                                       \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    /* cmp reg, imm; add, subtract */                                         \
    case 0x28 ... 0x3F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* TST, NEG, CMP, CMN */                                                  \
    case 0x42:                                                                \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* ORR, MUL, BIC, MVN */                                                  \
    case 0x43:                                                                \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov might change PC (fall through if so) */                            \
    case 0x46:                                                                \
      if((opcode & 0xFF87) != 0x4687)                                         \
        break;                                                                \
                                                                              \
    /* branches (can change PC) */                                            \
    case 0x47:                                                                \
    case 0xBD:                                                                \
    case 0xD0 ... 0xE7:                                                       \
    case 0xF0 ... 0xFF:                                                       \
      thumb_flag_requires_all();                                              \
      break;                                                                  \
  }                                                                           \
  block_data[block_data_position].flag_data = flag_status;                    \
}                                                                             \

// I/EWRAM memory tagging
// Code emitted in the RAM cache has tags (16 bit values) in the mirror tag ram
// that indicate that the address contains code. The following values are used:
// 0x0000 : this is just data (never translated)
// 0x00XX : not used (since first byte is zero)
// 0x0101 : this is code that is not the start of a translated block
// 0xXXXX : this is the start of a translated block, starting from 0xFFFF downwards
//          LSB is always set (we decrement by two) to ensure both bytes != 0
//
// The tag value is an index to a `ramtag_type` structure that sits at the end
// of the RAM CACHE (grows like a stack). For simplicity we start tags at 0xFFFF
// and grow like a stack.

#define LAST_TAG_NUM       0x0101
#define INITIAL_TOP_TAG    0xFFFF
#define CODE_TAG_BLOCK16   0x0101
#define CODE_TAG_BLOCK32   0x01010101

#define VALID_TAG(tagn) (tagn > LAST_TAG_NUM)

/* (The direct-link/stable-thunk cross-check, and every other illegal SMC flag
 * combination, is now asserted together in gpsp_profile.h.) */

#ifdef SMC_PARTIAL_SAFE
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)                                 \
  do { if (smc_partial_active)                                                \
    (te)->code_bytes_##type = (u32)(ram_translation_ptr - (blkptr));          \
  } while (0)
#elif defined(SMC_PARTIAL)
/* ram_translation_ptr has advanced past the block just emitted, so the size is
 * simply the distance from its entry point. */
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)                                 \
  (te)->code_bytes = (u32)(ram_translation_ptr - (blkptr))
#else
#define SMC_NOTE_CODE_BYTES(te, blkptr, type)  do { } while (0)
#endif

#define allocate_tag_arm(location) {   \
  location[0] = ram_block_tag;         \
  /* Could be another thumb inst */    \
  if (!location[1])                    \
    location[1] = CODE_TAG_BLOCK16;    \
  ram_block_tag -= 2;                  \
}

#define allocate_tag_thumb(location) { \
  location[0] = ram_block_tag;         \
  ram_block_tag -= 2;                  \
}

typedef struct
{
  u32 offset_arm;     // Cache offset to the ARM-mode public entry
  u32 offset_thumb;   // Cache offset to the Thumb-mode public entry
#ifdef SMC_PARTIAL_SAFE
  /* The same guest address can have both ARM and Thumb translations. Keep
   * their source extents separate so translating one mode cannot hide an
   * overlapping block in the other mode from an SMC retirement. */
  u32 blk_end_arm;
  u32 blk_end_thumb;
  u32 code_bytes_arm;
  u32 code_bytes_thumb;
#elif defined(SMC_PARTIAL)
  /* Source extent [start, end) of this block, recorded by
   * ramtag_note_extent() as soon as scan_block settles it.  Partial
   * invalidation needs it to answer "does this block actually span the
   * written address?" — without it the retire path can only guess from tag
   * adjacency, and guessing either misses overlapping blocks (v1: fast but
   * corrupted audio) or retires far too many (v2: correct but slower than
   * the full flush it replaced). */
  u32 blk_start;
  u32 blk_end;
  u32 code_bytes;   /* translated size, so the retire path knows whether an
                     * entry-point trampoline fits without clobbering the
                     * block emitted after it */
#endif
} ramtag_type;

#ifdef SMC_PARTIAL_SAFE
#define SMC_INIT_TAG_METADATA(te) do {                                        \
  if (smc_partial_active) {                                                   \
    (te)->blk_end_arm = 0;                                                    \
    (te)->blk_end_thumb = 0;                                                  \
    (te)->code_bytes_arm = 0;                                                 \
    (te)->code_bytes_thumb = 0;                                               \
  }                                                                           \
} while (0)
#else
#define SMC_INIT_TAG_METADATA(te) do { } while (0)
#endif

static u32 ram_block_tag = INITIAL_TOP_TAG;

inline static ramtag_type* get_ram_tag(u16 tagval) {
  ramtag_type *tbl = (ramtag_type*)&ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE];
  s16 tgidx = (s16)(tagval);
  return &tbl[tgidx >> 1];  /* Since LSB is always 1 and thus unused */
}

#ifdef SMC_PARTIAL_SAFE
u32 smc_partial_active;

/* Selective retirement temporarily clears a hot patch domain's tag bytes so
 * the remaining stores in the same mixer patch batch do not each trap. Keep
 * its block-start tags here and restore them on the next real lookup, avoiding
 * consumption of a new tag number on every audio tick. */
#define SMC_RETIRED_TAGS 512
typedef struct { u32 addr; u16 tag; } smc_retired_tag_type;
static smc_retired_tag_type smc_retired_tags[SMC_RETIRED_TAGS];
static u32 smc_retired_tag_count;

static void smc_restore_retired_tag(u32 pc, u16 *tagp, u32 thumb)
{
  u32 i;
  u32 addr = ((pc >> 24) == 3) ? 0x03000000 + (pc & 0x7FFF)
                               : 0x02000000 + (pc & 0x3FFFF);
  if (*tagp)
    return;
  for (i = 0; i < smc_retired_tag_count; i++)
    if (smc_retired_tags[i].addr == addr) {
      *tagp = smc_retired_tags[i].tag;
      if (!thumb && !tagp[1])
        tagp[1] = CODE_TAG_BLOCK16;
      return;
    }
}

static int smc_remember_retired_tag(u32 addr, u16 tag)
{
  u32 i;
  for (i = 0; i < smc_retired_tag_count; i++)
    if (smc_retired_tags[i].addr == addr) {
      smc_retired_tags[i].tag = tag;
      return 1;
    }
  if (smc_retired_tag_count == SMC_RETIRED_TAGS)
    return 0;
  smc_retired_tags[smc_retired_tag_count].addr = addr;
  smc_retired_tags[smc_retired_tag_count].tag = tag;
  smc_retired_tag_count++;
  return 1;
}

#ifdef SMC_PARTIAL_STABLE_THUNK
/* A stable thunk is the address returned to every RAM caller.  Its first two
 * words are either a direct jump to the current body or, while retired, the
 * first half of a four-instruction dispatcher path. */
static void smc_stable_thunk_link(u8 *entry, u8 *body)
{
  generate_branch_patch_unconditional(entry, body);
  address32(entry, 4) = 0;
  platform_cache_sync(entry, entry + 8);
}

static void smc_stable_thunk_dispatch(u8 *entry, u32 pc, u32 thumb)
{
  u8 *translation_ptr = entry;
  generate_load_imm(reg_a0, pc);
  if (thumb) {
    mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
  } else {
    mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
  }
  mips_emit_nop();
  platform_cache_sync(entry, translation_ptr);
}

#define SMC_RAM_NEEDS_TRANSLATION(te, type)                                   \
  (smc_partial_active ? !(te)->code_bytes_##type : !(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body) do {                       \
  if (smc_partial_active) {                                                   \
    if (!(te)->offset_##type)                                                 \
      (te)->offset_##type = (u32)((body) - 16 - ram_translation_cache);       \
    /* Nonzero also breaks a recursive lookup cycle while this body emits. */ \
    (te)->code_bytes_##type = 1;                                              \
  } else {                                                                    \
    (te)->offset_##type = (u32)((body) - ram_translation_cache);              \
  }                                                                           \
} while (0)
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do {                        \
  if (smc_partial_active)                                                     \
    smc_stable_thunk_link(&ram_translation_cache[(te)->offset_##type],        \
                          (body));                                             \
} while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do {                 \
  if (smc_partial_active) {                                                   \
    (te)->code_bytes_##type = 0;                                              \
    smc_stable_thunk_dispatch(                                                \
      &ram_translation_cache[(te)->offset_##type], (pc), (is_thumb));         \
  }                                                                           \
} while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#define SMC_RAM_LIVE_OFFSET(te, type) ((te)->code_bytes_##type)
#ifdef SMC_PARTIAL_DIRECT_LINKS
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target) (!(ram_region))
#else
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target)                       \
  (!(ram_region) || smc_partial_target_is_gated(GBA_PC(target)))
#endif
#else
#define SMC_RAM_NEEDS_TRANSLATION(te, type) (!(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body)                           \
  ((te)->offset_##type = (u32)((body) - ram_translation_cache))
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do { } while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do { } while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#define SMC_RAM_LIVE_OFFSET(te, type) ((te)->offset_##type)
#define SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, target)                       \
  (!(ram_region) || smc_partial_target_is_gated(GBA_PC(target)))
#endif

#define SMC_RESTORE_RETIRED_TAG_arm(pc, tagp) \
  do { if (smc_partial_active) smc_restore_retired_tag((pc), (tagp), 0); } while (0)
#define SMC_RESTORE_RETIRED_TAG_thumb(pc, tagp) \
  do { if (smc_partial_active) smc_restore_retired_tag((pc), (tagp), 1); } while (0)
#else
#define SMC_RESTORE_RETIRED_TAG_arm(pc, tagp) do { } while (0)
#define SMC_RESTORE_RETIRED_TAG_thumb(pc, tagp) do { } while (0)
#define SMC_RAM_NEEDS_TRANSLATION(te, type) (!(te)->offset_##type)
#define SMC_RAM_PREPARE_TRANSLATION(te, type, body)                           \
  ((te)->offset_##type = (u32)((body) - ram_translation_cache))
#define SMC_RAM_COMMIT_TRANSLATION(te, type, body) do { } while (0)
#define SMC_RAM_ABORT_TRANSLATION(te, type, pc, is_thumb) do { } while (0)
#define SMC_RAM_PUBLIC_ENTRY(te, type)                                        \
  (&ram_translation_cache[(te)->offset_##type])
#endif

// This function will return a pointer to a translated block of code. If it
// doesn't exist it will translate it, if it does it will pass it back.

// type should be "arm", "thumb", or "dual." For arm or thumb the PC should
// be a real PC, for dual the least significant bit will determine if it's
// ARM or Thumb mode.

/* GBA_PC_MASK: the GBA decodes only address bits 24-27 -- GBATEK lists
 * 0x10000000-0xFFFFFFFF as "not used (upper 4bits of address bus unused)",
 * so on hardware 0x58581818 IS 0x08581818 and executes from ROM.  gpSP does
 * not mask, so such a pc misses every case of the region switch and falls
 * through to the ~0 sentinel, which nothing checks -- that is the "Bad
 * Execution Address / PC: ffffffff" crash.  It is also an out-of-bounds
 * index: memory_map_read[] is 8K entries for pc>>15 (28 bits exactly) and
 * def_seq_cycles[] is 16 entries for pc>>24.  Masking restores the hardware
 * behaviour and the array bounds in one go. */
#ifdef GBA_PC_MASK
#define GBA_PC(x) ((x) & 0x0FFFFFFFu)
#else
#define GBA_PC(x) (x)
#endif

#define block_lookup_address_pc_arm()                                         \
  u32 thumb = 0;                                                              \
  pc = GBA_PC(pc) & ~0x03

#define block_lookup_address_pc_thumb()                                       \
  u32 thumb = 1;                                                              \
  pc = GBA_PC(pc) & ~0x01                                                     \


/* What the dispatcher gets when the emulated PC is outside every region the
 * lookup handles.  Upstream returns ~0 and block_lookup_address_* passes it
 * straight through -- it only tests for NULL -- so mips_stub.S does `jr $2`
 * into 0xFFFFFFFF and the PSP dies with "Bad Execution Address".  That is
 * the crash signature we caught in PPSSPP.
 *
 * BADJUMP_REPORT keeps the fault identical (0xF_______ is just as unmapped)
 * but folds the offending 28-bit GBA pc into the address, so the crash
 * screen prints the value we actually need to diagnose it. */
/* THE VALUE block_lookup_translate_* RETURNS FOR A PC IT CANNOT TRANSLATE.
 *
 * Its switch handles regions 0x0 (BIOS), 0x2 (EWRAM), 0x3 (IWRAM) and
 * 0x8..0xD (ROM).  Anything else -- I/O, palette, VRAM, OAM, SRAM -- falls
 * through to this sentinel.  Upstream chose a non-NULL value deliberately, so
 * one garbage exit from a speculative scan does not abort an otherwise good
 * translation (see the comment at the return).  The hazard it created is that
 * (u8 *)(~0) is also a perfectly plausible-looking POINTER, and every caller
 * that only tests `if (ret)` accepts it. */
#define BLOCK_LOOKUP_UNMAPPABLE ((u8 *)(~0))

/* The same region set as a pure function of the target address.
 *
 * Needed because the translator has to classify a block exit BEFORE it commits
 * translation_ptr: at that point it can still emit a dispatch island into the
 * block's own budget, whereas by the time the eager patch loop runs the
 * pointer is published and there is nowhere left to put one. */
static inline int gba_pc_translatable(u32 pc)
{
  u32 region = GBA_PC(pc) >> 24;
  return region == 0x0 || region == 0x2 || region == 0x3 ||
         (region >= 0x8 && region <= 0xD);
}

/* Block exits routed through the dispatcher instead of being patched to
 * BLOCK_LOOKUP_UNMAPPABLE.  Every one of these is a `j 0x3FFFFFF` -- a jump to
 * 0x0FFFFFFC -- that a pre-fix build wrote into a live block, so the counter is
 * the measurement of the freeze exposure rather than a claim about it.  Cheap
 * enough to keep in the release build: it only moves during translation. */
u32 badjump_contained = 0;

#ifdef BADJUMP_REPORT
/* pc >> 4, not pc & 0x0FFFFFFF: the first attempt masked off the TOP nibble,
 * which is the very part that decides the region -- it printed f8581818 for
 * a pc whose region 0x8 the switch above actually handles, proving the real
 * pc had a high nibble set and that the mask had eaten it.  Shifting keeps
 * the top 28 bits and drops only the instruction-alignment nibble. */
/* The bad TARGET is already known -- 0x58581818, identically on two runs.
 * What is not known is who branched there, so report the previous block
 * entry instead.  Same fault, same timing, different field. */
extern u32 badjump_prev_pc;
void badjump_report(u32 pc);
/* One run, whole picture: dump everything interesting to ms0:/badjump.txt
 * (PPSSPP maps that to a host folder) and THEN return the sentinel, so the
 * fault is unchanged.  Diagnostic only -- BADJUMP_REPORT never ships. */
#define BADJUMP_SENTINEL(pc) (badjump_report((pc)), BLOCK_LOOKUP_UNMAPPABLE)
#else
#define BADJUMP_SENTINEL(pc) (BLOCK_LOOKUP_UNMAPPABLE)
#endif

#define block_lookup_translate_builder(type)                                  \
u8 function_cc *block_lookup_translate_##type(u32 pc)                         \
{                                                                             \
  u8 pcregion = (GBA_PC(pc) >> 24);                                           \
  u16 *location;                                                              \
  u32 block_tag;                                                              \
                                                                              \
  block_lookup_address_pc_##type();                                           \
                                                                              \
  switch(pcregion)                                                            \
  {                                                                           \
    case 0x2:                                                                 \
    case 0x3:                                                                 \
    {                                                                         \
      u16* tagp = (pcregion == 2) ? (u16 *)(ewram + (pc & 0x3FFFF) + 0x40000) \
                                  : (u16 *)(iwram + (pc & 0x7FFF));           \
      ramtag_type* trentry;                                                   \
      SMC_RESTORE_RETIRED_TAG_##type(pc, tagp);                               \
      /* Allocate a tag if not a valid one, and initialize header */          \
      if (!VALID_TAG(*tagp)) {                                                \
        allocate_tag_##type(tagp);                                            \
        trentry = get_ram_tag(*tagp);                                         \
        trentry->offset_arm = 0;                                              \
        trentry->offset_thumb = 0;                                            \
        SMC_INIT_TAG_METADATA(trentry);                                       \
      } else {                                                                \
        trentry = get_ram_tag(*tagp);                                         \
      }                                                                       \
                                                                              \
      if (SMC_RAM_NEEDS_TRANSLATION(trentry, type)) {                         \
        bool result;                                                          \
        u32 cph_t;                                                            \
        u8 *blkptr = ram_translation_ptr + ram_block_prologue_size;           \
        SMC_RAM_PREPARE_TRANSLATION(trentry, type, blkptr);                   \
        /* Phase 5h: dynarec COMPILATION, separated from execution.  A        \
         * few tens of calls a frame in steady state, and the bracket is      \
         * nesting-safe because translate_block re-enters itself through      \
         * block_lookup_translate below -- see main.h. */                     \
        cph_t = core_phase_enter(CORE_PHASE_FINE);                            \
        result = translate_block_##type(pc, true);                            \
        core_phase_leave(CORE_PHASE_FINE, &cph_jit, cph_t);                   \
                                                                              \
        if (result) {                                                         \
          SMC_NOTE_CODE_BYTES(trentry, blkptr, type);                         \
          SMC_RAM_COMMIT_TRANSLATION(trentry, type, blkptr);                  \
          return SMC_RAM_PUBLIC_ENTRY(trentry, type);                         \
        }                                                                     \
        SMC_RAM_ABORT_TRANSLATION(trentry, type, pc, thumb);                  \
      } else {                                                                \
        return SMC_RAM_PUBLIC_ENTRY(trentry, type);                           \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
                                                                              \
    case 0x0:                                                                 \
    case 0x8 ... 0xD:                                                         \
    {                                                                         \
      u32 key = pc | thumb;                                                   \
      u32 hash_target = ((key * 2654435761U) >> (32 - ROM_BRANCH_HASH_BITS))  \
                                              & (ROM_BRANCH_HASH_SIZE - 1);   \
                                                                              \
      hashhdr_type *bhdr;                                                     \
      u32 blk_offset = rom_branch_hash[hash_target];                          \
      u32 *blk_offset_addr = &rom_branch_hash[hash_target];                   \
      while(blk_offset)                                                       \
      {                                                                       \
        bhdr = (hashhdr_type*)&rom_translation_cache[blk_offset];             \
        if(bhdr->pc_value == key)                                             \
          return &rom_translation_cache[                                      \
                  blk_offset + sizeof(hashhdr_type) + block_prologue_size];   \
                                                                              \
        blk_offset = bhdr->next_entry;                                        \
        blk_offset_addr = &bhdr->next_entry;                                  \
      }                                                                       \
                                                                              \
      { /* Not found, go ahead and translate, and backfill the hash table */  \
        u8 *blkptr;                                                           \
        bool result;                                                          \
        u32 cph_t;                                                            \
        bhdr = (hashhdr_type*)rom_translation_ptr;                            \
        bhdr->pc_value = key;                                                 \
        bhdr->next_entry = 0;                                                 \
        *blk_offset_addr = (u32)(rom_translation_ptr - rom_translation_cache);\
        rom_translation_ptr += sizeof(hashhdr_type);                          \
        blkptr = rom_translation_ptr + block_prologue_size;                   \
        cph_t = core_phase_enter(CORE_PHASE_FINE);                            \
        result = translate_block_##type(pc, false);                           \
        core_phase_leave(CORE_PHASE_FINE, &cph_jit, cph_t);                   \
                                                                              \
        if (result)                                                           \
          return blkptr;   /* ROM block: never retired, no size needed */     \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
  }                                                                           \
                                                                              \
  /* Do not return NULL since it could indeed happen that some branch         \
     points to some random place (perhaps due to being garbage). This can     \
     happen when especulatively compiling code in RAM. Perhaps the game       \
     patches these instructions later, which would trigger a flush */         \
  return BADJUMP_SENTINEL(pc);                                                \
}                                                                             \

block_lookup_translate_builder(arm);
block_lookup_translate_builder(thumb);

#ifdef SMC_PARTIAL_SAFE
static int smc_partial_target_is_gated(u32 addr)
{
  u32 i;
  for (i = 0; i < translation_gate_targets; i++)
    if (translation_gate_target_pc[i] == addr)
      return 1;
  return 0;
}
#endif

u8 function_cc *block_lookup_address_dual(u32 pc)
{
  u32 thumb = pc & 0x01;
  if(thumb) {
    pc &= ~1;
    reg[REG_CPSR] |= 0x20;
    return block_lookup_address_thumb(pc);
  } else {
    pc = (pc + 2) & ~0x03;
    reg[REG_CPSR] &= ~0x20;
    return block_lookup_address_arm(pc);
  }
}

#ifdef BADJUMP_REPORT
u32 badjump_prev_pc;   /* pc of the lookup before the current one */
#define BADJUMP_NOTE_PC(p) do { badjump_prev_pc = (p); } while (0)
#else
#define BADJUMP_NOTE_PC(p) do { } while (0)
#endif

#ifdef BADJUMP_REPORT
u32 badjump_prev_pc;
void badjump_report(u32 pc)
{
  static int seen = 0;
  FILE *f = fopen("ms0:/badjump.txt", "a");
  unsigned g;
  if (!f) return;
  fprintf(f, "=== badjump #%d ===\n", ++seen);
  fprintf(f, "bad target pc  : %08x\n", (unsigned)pc);
  fprintf(f, "prev lookup pc : %08x\n", (unsigned)badjump_prev_pc);
  fprintf(f, "smc_last_write : %08x\n", (unsigned)smc_last_write_addr);
  fprintf(f, "cpsr           : %08x  (T=%u)\n", (unsigned)reg[REG_CPSR],
          (unsigned)((reg[REG_CPSR] >> 5) & 1));
  fprintf(f, "gates (%u):", (unsigned)translation_gate_targets);
  for (g = 0; g < translation_gate_targets; g++)
    fprintf(f, " %08x", (unsigned)translation_gate_target_pc[g]);
  fprintf(f, "\n");
  for (g = 0; g < 16; g++)                 /* r13=SP r14=LR r15=PC */
    fprintf(f, "r%-2u=%08x%s", g, (unsigned)reg[g],
            (g % 4 == 3) ? "\n" : "  ");
  fprintf(f, "\nflush ram total %u smc %u\n",
          (unsigned)flush_ram_total, (unsigned)flush_ram_smc);
  fclose(f);
}
#endif
/* BADJUMP_SAFE: an unmappable guest pc must never reach `jr $v0`.
 *
 * block_lookup_translate returns (u8*)(~0) when the pc is outside every
 * region it handles, and NULL when translation failed 4x.  Neither is checked
 * anywhere: block_lookup_address_* only tests `if (ret)`, so ~0 sails through,
 * and mips_stub.S mips_indirect_branch_* does `jr $v0` straight into it.  That
 * is the hard PSP crash -- "Bad Execution Address, PC: ffffffff" (or 00000000
 * once the pc is masked).  Verified in the disassembly: the crash RA lands at
 * mips_indirect_branch_arm+0x44, the instruction after the JAL to
 * block_lookup_address_arm.
 *
 * Garbage branch targets are ROUTINE, not exceptional -- 223 in one battle,
 * every one from a block a gate started at a non-instruction boundary.  Almost
 * none are ever executed; the crash is the rare one that is.  The guest has
 * already gone wrong by the time we arrive, so there is nothing to preserve:
 * treat it as a guest fault and soft-reset the GBA instead of taking the
 * console down.  Emulator, frontend and memory stick all survive.
 *
 * This cannot regress anything -- the path it replaces crashes 100% of the
 * time, so there is no working behaviour to lose. */
#ifdef BADJUMP_SAFE
u32 badjump_recoveries = 0;
static u8 *badjump_recover(u32 pc)
{
  u8 *r;
  badjump_recoveries++;
#ifdef BADJUMP_REPORT
  /* DIAGNOSTIC BUILDS ONLY.  This is reached from mips_indirect_branch_* --
   * i.e. from inside a translated block, with the dynarec's register state
   * live -- so fopen/fprintf/fclose here is a synchronous Memory Stick write
   * on the emulation thread.  It also wrote ms0:/badjump.txt out of the
   * shipped 7283f13 candidate, which the release audit's forbidden-string
   * list did not cover. */
  {
    FILE *f = fopen("ms0:/badjump.txt", "a");
    if (f) {
      fprintf(f, "*** RECOVERED #%u: guest pc %08x was unmappable, GBA reset\n",
              (unsigned)badjump_recoveries, (unsigned)pc);
      fclose(f);
    }
  }
#endif
  reg[REG_CPSR] &= ~0x20;            /* ARM mode */
  reg[REG_PC]    = 0x0;              /* GBA reset vector */
  r = block_lookup_translate_arm(0x0);
  if (r && r != BLOCK_LOOKUP_UNMAPPABLE)
    return r;
  return bios_swi_entrypoint;        /* last resort: known-good block */
}
#define BADJUMP_GUARD(ret, pc)                                            \
  do { if ((ret) == BLOCK_LOOKUP_UNMAPPABLE) return badjump_recover(pc); } while (0)
#define BADJUMP_EXHAUSTED(pc)  return badjump_recover(pc)
#else
#define BADJUMP_GUARD(ret, pc) do { } while (0)
#define BADJUMP_EXHAUSTED(pc)  return NULL
#endif

u8 function_cc *block_lookup_address_arm(u32 pc)
{
  unsigned i;
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_arm(pc);
    BADJUMP_GUARD(ret, pc);
    if (ret) {
      translate_icache_sync(); BADJUMP_NOTE_PC(pc);
      return ret;
    }
  }

  /* Same reason as badjump_recover: stdio from the dispatch path blocks the
   * emulation thread on real hardware.  Diagnostic builds only. */
#ifdef BADJUMP_REPORT
  printf("bad jump %x (%x)\n", (unsigned)pc, (unsigned)reg[REG_PC]);
  fflush(stdout);
#endif
  BADJUMP_EXHAUSTED(pc);
}

u8 function_cc *block_lookup_address_thumb(u32 pc)
{
  unsigned i;
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_thumb(pc);
    BADJUMP_GUARD(ret, pc);
    if (ret) {
      translate_icache_sync(); BADJUMP_NOTE_PC(pc);
      return ret;
    }
  }
  /* Same reason as badjump_recover: stdio from the dispatch path blocks the
   * emulation thread on real hardware.  Diagnostic builds only. */
#ifdef BADJUMP_REPORT
  printf("bad jump %x (%x)\n", (unsigned)pc, (unsigned)reg[REG_PC]);
  fflush(stdout);
#endif
  BADJUMP_EXHAUSTED(pc);
}


// Potential exit point: If the rd field is pc for instructions is 0x0F,
// the instruction is b/bl/bx, or the instruction is ldm with PC in the
// register list.
// All instructions with upper 3 bits less than 100b have an rd field
// except bx, where the bits must be 0xF there anyway, multiplies,
// which cannot have 0xF in the corresponding fields, and msr, which
// has 0x0F there but doesn't end things (therefore must be special
// checked against). Because MSR and BX overlap both are checked for.

#define arm_exit_point                                                        \
 (((opcode < 0x8000000) && ((opcode & 0x000F000) == 0x000F000) &&             \
  ((opcode & 0xDB0F000) != 0x120F000)) ||                                     \
  ((opcode & 0x12FFF10) == 0x12FFF10) ||                                      \
  ((opcode & 0x8108000) == 0x8108000) ||                                      \
  ((opcode >= 0xA000000) && (opcode < 0xF000000)) ||                          \
  ((opcode >= 0xF000000) && (!is_div_swi((opcode >> 16) & 0xFF))))            \

#define arm_opcode_branch                                                     \
  ((opcode & 0xE000000) == 0xA000000)                                         \

#define arm_opcode_swi                                                        \
  ((opcode & 0xF000000) == 0xF000000)                                         \

#define arm_opcode_unconditional_branch                                       \
  (condition == 0x0E)                                                         \

#define arm_load_opcode()                                                     \
  opcode = readaddress32(pc_address_block, (block_end_pc & 0x7FFF));          \
  condition = opcode >> 28;                                                   \
                                                                              \
  opcode &= 0xFFFFFFF;                                                        \
                                                                              \
  block_end_pc += 4                                                           \

#define arm_branch_target()                                                   \
  branch_target = (block_end_pc + 4 + (((s32)(opcode & 0xFFFFFF) << 8) >> 6)) \

// Contiguous conditional block flags modification - it will set 0x20 in the
// condition's bits if this instruction modifies flags. Taken from the CPU
// switch so it'd better be right this time.

#define arm_set_condition(_condition)                                         \
  block_data[block_data_position].condition = _condition;                     \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x01:                                                                \
    case 0x03:                                                                \
    case 0x09:                                                                \
    case 0x0B:                                                                \
    case 0x0D:                                                                \
    case 0x0F:                                                                \
      if((((opcode >> 5) & 0x03) == 0) || ((opcode & 0x90) != 0x90))          \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
    case 0x07:                                                                \
    case 0x11:                                                                \
    case 0x13:                                                                \
    case 0x15 ... 0x17:                                                       \
    case 0x19:                                                                \
    case 0x1B:                                                                \
    case 0x1D:                                                                \
    case 0x1F:                                                                \
      if((opcode & 0x90) != 0x90)                                             \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if(((opcode & 0x90) != 0x90) && !(opcode & 0x10))                       \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
    case 0x23:                                                                \
    case 0x25:                                                                \
    case 0x27:                                                                \
    case 0x29:                                                                \
    case 0x2B:                                                                \
    case 0x2D:                                                                \
    case 0x2F ... 0x37:                                                       \
    case 0x39:                                                                \
    case 0x3B:                                                                \
    case 0x3D:                                                                \
    case 0x3F:                                                                \
      block_data[block_data_position].condition |= 0x20;                      \
    break;                                                                    \
  }                                                                           \

#define arm_instruction_width 4

#define arm_base_cycles()                                                     \
  cycle_count += def_seq_cycles[pc >> 24][1]                                  \

// For now this just sets a variable that says flags should always be
// computed.

#define arm_dead_flag_eliminate()                                             \
  flag_status = 0xF                                                           \

// The following Thumb instructions can exit:
// b, bl, bx, swi, pop {... pc}, and mov pc, ..., the latter being a hireg
// op only. Rather simpler to identify than the ARM set.

#define thumb_exit_point                                                      \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   (((opcode & 0xFF00) == 0xDF00) &&                                          \
    (!is_div_swi(opcode & 0xFF))) ||                                          \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   ((opcode & 0xFF00) == 0x4700) ||                                           \
   ((opcode & 0xFF00) == 0xBD00) ||                                           \
   ((opcode & 0xFF87) == 0x4687) ||                                           \
   ((opcode >= 0xF800)))                                                      \

#define thumb_opcode_branch                                                   \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   (opcode >= 0xF800))                                                        \

#define thumb_opcode_swi                                                      \
  ((opcode & 0xFF00) == 0xDF00)                                               \

#define thumb_opcode_unconditional_branch                                     \
  ((opcode < 0xD000) || (opcode >= 0xDF00))                                   \

#define thumb_load_opcode()                                                   \
  last_opcode = opcode;                                                       \
  opcode = readaddress16(pc_address_block, (block_end_pc & 0x7FFF));          \
                                                                              \
  block_end_pc += 2                                                           \

#define thumb_branch_target()                                                 \
  if(opcode < 0xE000)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s8)(opcode & 0xFF) * 2);             \
  }                                                                           \
  else                                                                        \
                                                                              \
  if(opcode < 0xF800)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s32)((opcode & 0x7FF) << 21) >> 20); \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                     \
    {                                                                         \
      branch_target =                                                         \
       (block_end_pc + ((s32)((last_opcode & 0x07FF) << 21) >> 9) +           \
       ((opcode & 0x07FF) * 2));                                              \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      goto no_direct_branch;                                                  \
    }                                                                         \
  }                                                                           \

#define thumb_set_condition(_condition)                                       \

#define thumb_instruction_width 2

#define thumb_base_cycles()                                                   \
  cycle_count += def_seq_cycles[pc >> 24][0]                                  \

// Here's how this works: each instruction has three different sets of flag
// attributes, each consisiting of a 4bit mask describing how that instruction
// interacts with the 4 main flags (N/Z/C/V).
// The first set, in bits 0:3, is the set of flags the instruction may
// modify. After this pass this is changed to the set of flags the instruction
// should modify - if the bit for the corresponding flag is not set then code
// does not have to be generated to calculate the flag for that instruction.

// The second set, in bits 7:4, is the set of flags that the instruction must
// modify (ie, for shifts by the register values the instruction may not
// always modify the C flag, and thus the C bit won't be set here).

// The third set, in bits 11:8, is the set of flags that the instruction uses
// in its computation, or the set of flags that will be needed after the
// instruction is done. For any instructions that change the PC all of the
// bits should be set because it is (for now) unknown what flags will be
// needed after it arrives at its destination. Instructions that use the
// carry flag as input will have it set as well.

// The algorithm is a simple liveness analysis procedure: It starts at the
// bottom of the instruction stream and sets a "currently needed" mask to
// the flags needed mask of the current instruction. Then it moves down
// an instruction, ANDs that instructions "should generate" mask by the
// "currently needed" mask, then ANDs the "currently needed" mask by
// the 1's complement of the instruction's "must generate" mask, and ORs
// the "currently needed" mask by the instruction's "flags needed" mask.

#define thumb_dead_flag_eliminate()                                           \
{                                                                             \
  u32 needed_mask = 0xff;                                                     \
                                                                              \
  while(--block_data_position >= 0)                                           \
  {                                                                           \
    flag_status = block_data[block_data_position].flag_data;                  \
    block_data[block_data_position].flag_data =                               \
     (flag_status & needed_mask);                                             \
    needed_mask &= ~((flag_status >> 4) & 0x0F);                              \
    needed_mask |= flag_status >> 8;                                          \
  }                                                                           \
}                                                                             \

#define MAX_BLOCK_SIZE   1024   // 2/4KiB blocks max
#define MAX_EXITS          32   // This covers 99% blocks

/* WHERE AN UNMAPPABLE BLOCK EXIT IS SENT, AND WHY IT EMITS NOTHING.
 *
 * The first version of this fix emitted a four-instruction dispatch island per
 * untranslatable exit -- load the guest pc, jump to mips_indirect_branch_*.  It
 * was correct and it was expensive: measured on hardware at +16.4% frame work on
 * heart_soul_light and +8.2% on unbound_double_high, both consoles.  The reason
 * is a feedback loop rather than the loop's own cost.  Every island is 16 bytes
 * of GENERATED code in the RAM translation cache, on workloads that already
 * re-translate constantly; the cache fills sooner, a full flush retires
 * everything, re-translation emits the islands again, and round.  The counter
 * shows it directly: xlat 705 -> 831 in the same window.
 *
 * So exits go to bios_swi_entrypoint instead, and nothing is emitted at all --
 * one patched word per exit.  That block:
 *
 *   * is pre-generated once by init_bios_hooks() BELOW rom_cache_watermark, so
 *     it survives every ROM and RAM cache flush and its address never moves;
 *   * is already designated the safe fallback by this file -- badjump_recover
 *     returns it as its "last resort: known-good block";
 *   * is already patched into RAM blocks by both translate_block_* for a
 *     branch_target of 0x00000008, so a RAM->ROM direct link to it is an
 *     established pattern and not a new hazard.
 *
 * The invariant is unchanged and is the whole point: a translated block never
 * contains a direct jump to an address the translator could not resolve.  An
 * executed garbage branch now enters the BIOS SWI handler, which is defined
 * behaviour for the console even though the guest has already gone wrong -- as
 * against 7283f13, where it fetched from 0x0FFFFFFC and the PSP died.
 *
 * (The emit budget this used to reserve is gone with the islands.  Measured
 * separately: reserving it cost nothing, so it was never the problem -- but
 * with nothing emitted there is nothing to reserve for.) */

block_data_type block_data[MAX_BLOCK_SIZE];
block_exit_type block_exits[MAX_EXITS];

#define smc_write_arm_yes() {                                                 \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK32;                                                       \
  }                                                                           \
}

#define smc_write_thumb_yes() {                                               \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
  }                                                                           \
}

#define smc_write_arm_no()                                                    \

#define smc_write_thumb_no()                                                  \

/* SMC_SCAN_SPCLAMP: stop scanning at the STACK POINTER, not at the top of
 * IWRAM.
 *
 * The stock clamp is block_end_pc == 0x3007FF0 -- the last words of IWRAM --
 * so a scan starting in IWRAM code runs forward through the whole live stack
 * and tags every byte of it as code (SMC_SCAN_RAMEND names exactly this).
 * Once the stack is tagged, ordinary PUSH/STM traffic trips the SMC check and
 * forces a FULL cache flush: one PPSSPP battle measured 10122 SMC flushes,
 * and six of eight gate slots had been spent within a few hundred bytes of SP.
 *
 * Code does not live above SP, so stopping there costs nothing real, and the
 * clamp is safe by construction: worst case is an earlier block boundary,
 * which the translator already handles everywhere.
 *
 * MEASURED: NULL RESULT, do not retry as-is.  The stack grows DOWN, so the
 * live stack sits ABOVE sp and pushes land just BELOW it -- this clamp
 * protects [sp, 0x3008000), which is the wrong side.  The gated stack
 * addresses seen in the field (0x3007a38..0x3007c64) were all below an sp
 * of 0x3007c6c, so the clamp never covered them: s/x were unchanged
 * (s234 x7101 vs s242 x7355) and the crash still landed on b1c9. */
#ifdef SMC_SCAN_SPCLAMP
#define SMC_SCAN_PAST_SP(endpc)                                            \
  ((endpc) >= 0x3000000 && (endpc) < 0x3008000 &&                          \
   reg[REG_SP] >= 0x3000000 && reg[REG_SP] < 0x3008000 &&                  \
   (endpc) >= reg[REG_SP])
#else
#define SMC_SCAN_PAST_SP(endpc) (0)
#endif

#ifdef SMC_WRITE_HISTO
void smc_cover_note(u32 s, u32 e, u32 reason);
#endif

#if defined(SMC_GATE_BITMAP) && defined(SMC_GATES)
/* One bit per RAM halfword. This is a compiled view of the existing gate
 * table: it changes the cost of asking "is this PC gated?", never which PCs
 * are gates or where translated blocks end. */
static u8 smc_gate_iwram[0x8000 >> 4];
static u8 smc_gate_ewram[0x40000 >> 4];

static void smc_gate_map_add(u32 pc)
{
  u32 off;
  u8 *map;
  if ((pc >> 24) == 3) {
    off = (pc & 0x7FFF) >> 1;
    map = smc_gate_iwram;
  } else if ((pc >> 24) == 2) {
    off = (pc & 0x3FFFF) >> 1;
    map = smc_gate_ewram;
  } else {
    return;
  }
  map[off >> 3] |= (u8)(1u << (off & 7));
}

static void smc_gate_map_rebuild(void)
{
  u32 i;
  memset(smc_gate_iwram, 0, sizeof(smc_gate_iwram));
  memset(smc_gate_ewram, 0, sizeof(smc_gate_ewram));
  for (i = 0; i < translation_gate_targets; i++)
    smc_gate_map_add(translation_gate_target_pc[i]);
}

static inline int smc_gate_map_has(u32 pc)
{
  u32 off;
  const u8 *map;
  if ((pc >> 24) == 3) {
    off = (pc & 0x7FFF) >> 1;
    map = smc_gate_iwram;
  } else if ((pc >> 24) == 2) {
    off = (pc & 0x3FFFF) >> 1;
    map = smc_gate_ewram;
  } else {
    return 0;
  }
  return map[off >> 3] & (1u << (off & 7));
}

#define SMC_GATE_MAP_ADD(pc)       smc_gate_map_add(pc)
#define SMC_GATE_MAP_REBUILD()     smc_gate_map_rebuild()
#define SMC_SCAN_GATE_END() do {                                           \
  if (smc_gate_map_has(block_end_pc)) {                                    \
    scan_exit_reason = SMC_SCAN_GATE;                                      \
    goto block_end;                                                        \
  }                                                                        \
} while (0)
#else
#define SMC_GATE_MAP_ADD(pc)       do { } while (0)
#define SMC_GATE_MAP_REBUILD()     do { } while (0)
#define SMC_SCAN_GATE_END() do {                                           \
  for (i = 0; i < translation_gate_targets; i++) {                          \
    if (block_end_pc == translation_gate_target_pc[i]) {                   \
      scan_exit_reason = SMC_SCAN_GATE;                                    \
      goto block_end;                                                      \
    }                                                                      \
  }                                                                        \
} while (0)
#endif

#define scan_block(type, smc_write_op)                                        \
{                                                                             \
  __label__ block_end;                                                        \
  /* Find the end of the block */                                             \
  do                                                                          \
  {                                                                           \
    check_pc_region(block_end_pc);                                            \
    smc_write_##type##_##smc_write_op();                                      \
    type##_load_opcode();                                                     \
    type##_flag_status();                                                     \
                                                                              \
    if(type##_exit_point)                                                     \
    {                                                                         \
      /* Branch/branch with link */                                           \
      if(type##_opcode_branch)                                                \
      {                                                                       \
        __label__ no_direct_branch;                                           \
        type##_branch_target();                                               \
        block_exits[block_exit_position].branch_target = branch_target;       \
        block_exit_position++;                                                \
                                                                              \
        /* Give the branch target macro somewhere to bail if it turns out to  \
           be an indirect branch (ala malformed Thumb bl) */                  \
        no_direct_branch:;                                                    \
      }                                                                       \
                                                                              \
      /* SWI branches to the BIOS, unless it's an HLE call, then it is        \
         not parsed as an exit_point but rather an "instruction" of sorts. */ \
      if(type##_opcode_swi)                                                   \
      {                                                                       \
        block_exits[block_exit_position].branch_target = 0x00000008;          \
        block_exit_position++;                                                \
      }                                                                       \
                                                                              \
      type##_set_condition(condition | 0x10);                                 \
                                                                              \
      /* Only unconditional branches can end the block. */                    \
      if(type##_opcode_unconditional_branch)                                  \
      {                                                                       \
        /* Check to see if any prior block exits branch after here,           \
           if so don't end the block. Starts from the top and works           \
           down because the most recent branch is most likely to              \
           join after the end (if/then form) */                               \
        for(i = block_exit_position - 2; i >= 0; i--)                         \
        {                                                                     \
          if(block_exits[i].branch_target == block_end_pc)                    \
            break;                                                            \
        }                                                                     \
                                                                              \
        if(i < 0)                                                             \
        {                                                                     \
          scan_exit_reason = SMC_SCAN_UNCOND;                                 \
          break;                                                              \
        }                                                                     \
      }                                                                       \
      if(block_exit_position == MAX_EXITS)                                    \
      {                                                                       \
        scan_exit_reason = SMC_SCAN_MAXEXIT;                                  \
        break;                                                                \
      }                                                                       \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      type##_set_condition(condition);                                        \
    }                                                                         \
                                                                              \
    SMC_SCAN_GATE_END();                                                      \
                                                                              \
    block_data[block_data_position].update_cycles = 0;                        \
    block_data_position++;                                                    \
    if((block_data_position == MAX_BLOCK_SIZE) ||                             \
     (block_end_pc == 0x3007FF0) || (block_end_pc == 0x203FFFF0) ||           \
     SMC_SCAN_PAST_SP(block_end_pc))                                          \
    {                                                                         \
      scan_exit_reason = (block_data_position == MAX_BLOCK_SIZE)              \
                       ? SMC_SCAN_MAXSIZE : SMC_SCAN_RAMEND;                  \
      break;                                                                  \
    }                                                                         \
  } while(1);                                                                 \
                                                                              \
  block_end:;                                                                 \
}                                                                             \

#define arm_fix_pc()                                                          \
  pc &= ~0x03                                                                 \

#define thumb_fix_pc()                                                        \
  pc &= ~0x01                                                                 \

#define update_pc_limits()                                                    \
if (ram_region) {                                                             \
  if (pc >= 0x3000000) {                                                      \
    iwram_code_min = MIN(pc & 0x7FFF, iwram_code_min);                        \
    iwram_code_max = MAX(pc & 0x7FFF, iwram_code_max);                        \
  } else {                                                                    \
    ewram_code_min = MIN(pc & 0x3FFFF, ewram_code_min);                       \
    ewram_code_max = MAX(pc & 0x3FFFF, ewram_code_max);                       \
  }                                                                           \
}                                                                             \

bool translate_block_arm(u32 pc, bool ram_region)
{
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 last_condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 scan_exit_reason = 0;   /* phase 5g diagnostic: why the scan stopped */
#ifdef SMC_PARTIAL_DIRECT_GATE
  u8 *smc_gate_branch_source = NULL;
  u32 smc_gate_branch_target = 0;
#endif
  block_exit_type external_block_exits[MAX_EXITS];
  generate_block_extra_vars_arm();
  arm_fix_pc();

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       RAM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit =
     rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE -
     TRANSLATION_CACHE_LIMIT_THRESHOLD;
  }

  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(arm, yes);
    /* scan_block has just tagged exactly [block_start_pc, block_end_pc). */
    smc_blk_note_block(block_start_pc, block_end_pc, 0, scan_exit_reason);
#ifdef SMC_WRITE_HISTO
    smc_cover_note(block_start_pc, block_end_pc, scan_exit_reason);
#endif
  }
  else
  {
    scan_block(arm, no);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       arm_instruction_width].update_cycles = 1;
    }
  }

  arm_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  last_condition = 0x0E;

  while(pc != block_end_pc)
  {
    block_data[block_data_position].block_offset = translation_ptr;
    arm_base_cycles();

    if (pc == cheat_master_hook)
    {
      arm_process_cheats();
    }

    update_pc_limits();
    translate_arm_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit) {
      if (ram_region) {
        flush_ram_full++;
        flush_translation_cache_ram();
      }
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* This can happen if the last instruction is *not* inconditional */
  if ((last_condition & 0x0F) != 0x0E) {
    generate_branch_patch_conditional(backpatch_address, translation_ptr);
  }

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
#ifdef SMC_PARTIAL_DIRECT_GATE
  if (ram_region && smc_partial_active && scan_exit_reason == SMC_SCAN_GATE) {
    smc_gate_branch_target = pc;
    mips_emit_j_filler(smc_gate_branch_source);
    mips_emit_nop();
  } else
#endif
    generate_translation_gate(arm);

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        arm_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, save for later */
      external_block_exits[external_block_exit_position].branch_target =
       branch_target;
      external_block_exits[external_block_exit_position].branch_source =
       block_exits[i].branch_source;
      external_block_exit_position++;
    }
  }

#ifdef SMC_PARTIAL_SAFE
  /* ROM survives a RAM-cache flush, so a direct ROM->RAM jump could later
   * enter an overwritten cache slot. Route only those persistent links
   * through a local island. RAM->RAM links stay direct on the normal path;
   * their source disappears whenever the RAM cache is fully reset. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if (smc_partial_active &&
        SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, branch_target) &&
        (((GBA_PC(branch_target) >> 24) == 2) ||
         ((GBA_PC(branch_target) >> 24) == 3)))
    {
      translation_target = translation_ptr;
      generate_load_pc(reg_a0, GBA_PC(branch_target));
      mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
      mips_emit_nop();
      generate_branch_patch_unconditional(
        external_block_exits[i].branch_source, translation_target);
      external_block_exits[i].branch_source = NULL;
    }
  }
#endif

  /* AN UNTRANSLATABLE EXIT MUST NOT BE PATCHED IN AS A DIRECT JUMP.
   *
   * block_lookup_translate_* answers BLOCK_LOOKUP_UNMAPPABLE -- (u8 *)(~0) --
   * for a target outside the regions it handles, and the eager patch loop
   * below only ever tested for NULL.  The sentinel therefore reached
   * generate_branch_patch_unconditional, which emits
   *
   *     j ((0xFFFFFFFF / 4) & 0x3FFFFFF)   ==   j 0x3FFFFFF
   *
   * and MIPS `j` keeps the delay slot's top four PC bits, so the first
   * execution of that branch fetches from 0x0FFFFFFC.  Nothing is mapped
   * there on a PSP: the console takes an instruction-fetch fault with no
   * handler and freezes hard, with no dispatcher call anywhere in the path --
   * which is why BADJUMP_SAFE, which guards block_lookup_address_*, never saw
   * it and why the freeze survived that fix.
   *
   * Speculatively scanned blocks produce these targets routinely -- 223 in one
   * Heart & Soul battle, every one from a block a translation gate started at
   * a non-instruction boundary, and almost none ever executed.  So the rule is
   * containment rather than prediction: send the branch through the runtime
   * dispatcher.  That is lazy (an exit that is never taken costs nothing at
   * all) and it puts the fault back inside BADJUMP_SAFE's reach, where an
   * unmappable guest pc resets the GBA instead of the console.
   *
   * It runs HERE, before translation_ptr is published, because the island has
   * to come out of this block's emit budget; the region test is a pure
   * function of the target address, so nothing needs translating to decide.
   * Worst case is MAX_EXITS islands of 16 bytes = 512 B, and the loop above
   * can only claim an exit this one would (their region sets are disjoint), so
   * the two together stay inside TRANSLATION_CACHE_LIMIT_THRESHOLD's 2 KB. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if (branch_target == 0x00000008 || gba_pc_translatable(branch_target))
      continue;
    badjump_contained++;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, bios_swi_entrypoint);
    external_block_exits[i].branch_source = NULL;
  }

  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;

#ifdef SMC_PARTIAL_DIRECT_GATE
  if (smc_gate_branch_source) {
    translation_target = block_lookup_translate_arm(smc_gate_branch_target);
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    generate_branch_patch_unconditional(smc_gate_branch_source,
                                         translation_target);
  }
#endif

  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = block_lookup_translate_arm(branch_target);
    /* Backstop for the classifier above: the sentinel must never reach
     * generate_branch_patch_unconditional.  Aborting the translation is the
     * already-established safe path -- the caller retries, then BADJUMP_SAFE
     * resets the guest. */
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, translation_target);
  }
  return true;
}

bool translate_block_thumb(u32 pc, bool ram_region)
{
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 scan_exit_reason = 0;   /* phase 5g diagnostic: why the scan stopped */
#ifdef SMC_PARTIAL_DIRECT_GATE
  u8 *smc_gate_branch_source = NULL;
  u32 smc_gate_branch_target = 0;
#endif
  block_exit_type external_block_exits[MAX_EXITS];
  generate_block_extra_vars_thumb();
  thumb_fix_pc();

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       RAM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit = &rom_translation_cache[
       ROM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD];
  }

  generate_block_prologue();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(thumb, yes);
    smc_blk_note_block(block_start_pc, block_end_pc, 1, scan_exit_reason);
#ifdef SMC_WRITE_HISTO
    smc_cover_note(block_start_pc, block_end_pc, scan_exit_reason);
#endif
  }
  else
  {
    scan_block(thumb, no);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       thumb_instruction_width].update_cycles = 1;
    }
  }

  thumb_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  while(pc != block_end_pc)
  {
    block_data[block_data_position].block_offset = translation_ptr;
    thumb_base_cycles();

    if (pc == cheat_master_hook)
    {
      thumb_process_cheats();
    }

    update_pc_limits();
    translate_thumb_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    if(translation_ptr > translation_cache_limit)
    {
      if (ram_region) {
        flush_ram_full++;
        flush_translation_cache_ram();
      }
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* Unconditionally generate translation targets. In case we hit one or
     in the unlikely case that block was too big (and not finalized) */
#ifdef SMC_PARTIAL_DIRECT_GATE
  if (ram_region && smc_partial_active && scan_exit_reason == SMC_SCAN_GATE) {
    smc_gate_branch_target = pc;
    mips_emit_j_filler(smc_gate_branch_source);
    mips_emit_nop();
  } else
#endif
    generate_translation_gate(thumb);

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        thumb_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* External branch, save for later */
      external_block_exits[external_block_exit_position].branch_target =
       branch_target;
      external_block_exits[external_block_exit_position].branch_source =
       block_exits[i].branch_source;
      external_block_exit_position++;
    }
  }

#ifdef SMC_PARTIAL_SAFE
  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if (smc_partial_active &&
        SMC_RAM_LINK_NEEDS_DISPATCH(ram_region, branch_target) &&
        (((GBA_PC(branch_target) >> 24) == 2) ||
         ((GBA_PC(branch_target) >> 24) == 3)))
    {
      translation_target = translation_ptr;
      generate_load_pc(reg_a0, GBA_PC(branch_target));
      mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
      mips_emit_nop();
      generate_branch_patch_unconditional(
        external_block_exits[i].branch_source, translation_target);
      external_block_exits[i].branch_source = NULL;
    }
  }
#endif

  /* AN UNTRANSLATABLE EXIT MUST NOT BE PATCHED IN AS A DIRECT JUMP.
   *
   * block_lookup_translate_* answers BLOCK_LOOKUP_UNMAPPABLE -- (u8 *)(~0) --
   * for a target outside the regions it handles, and the eager patch loop
   * below only ever tested for NULL.  The sentinel therefore reached
   * generate_branch_patch_unconditional, which emits
   *
   *     j ((0xFFFFFFFF / 4) & 0x3FFFFFF)   ==   j 0x3FFFFFF
   *
   * and MIPS `j` keeps the delay slot's top four PC bits, so the first
   * execution of that branch fetches from 0x0FFFFFFC.  Nothing is mapped
   * there on a PSP: the console takes an instruction-fetch fault with no
   * handler and freezes hard, with no dispatcher call anywhere in the path --
   * which is why BADJUMP_SAFE, which guards block_lookup_address_*, never saw
   * it and why the freeze survived that fix.
   *
   * Speculatively scanned blocks produce these targets routinely -- 223 in one
   * Heart & Soul battle, every one from a block a translation gate started at
   * a non-instruction boundary, and almost none ever executed.  So the rule is
   * containment rather than prediction: send the branch through the runtime
   * dispatcher.  That is lazy (an exit that is never taken costs nothing at
   * all) and it puts the fault back inside BADJUMP_SAFE's reach, where an
   * unmappable guest pc resets the GBA instead of the console.
   *
   * It runs HERE, before translation_ptr is published, because the island has
   * to come out of this block's emit budget; the region test is a pure
   * function of the target address, so nothing needs translating to decide.
   * Worst case is MAX_EXITS islands of 16 bytes = 512 B, and the loop above
   * can only claim an exit this one would (their region sets are disjoint), so
   * the two together stay inside TRANSLATION_CACHE_LIMIT_THRESHOLD's 2 KB. */
  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if (branch_target == 0x00000008 || gba_pc_translatable(branch_target))
      continue;
    badjump_contained++;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, bios_swi_entrypoint);
    external_block_exits[i].branch_source = NULL;
  }

  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;

#ifdef SMC_PARTIAL_DIRECT_GATE
  if (smc_gate_branch_source) {
    translation_target = block_lookup_translate_thumb(smc_gate_branch_target);
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    generate_branch_patch_unconditional(smc_gate_branch_source,
                                         translation_target);
  }
#endif

  for(i = 0; i < external_block_exit_position; i++)
  {
    if (!external_block_exits[i].branch_source)
      continue;
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = block_lookup_translate_thumb(branch_target);
    /* Backstop for the classifier above: the sentinel must never reach
     * generate_branch_patch_unconditional.  Aborting the translation is the
     * already-established safe path -- the caller retries, then BADJUMP_SAFE
     * resets the guest. */
    if (!translation_target || translation_target == BLOCK_LOOKUP_UNMAPPABLE)
      return false;
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, translation_target);
  }
  return true;
}

void init_bios_hooks(void)
{
  // Pre-generate this entry point so that we can safely invoke fast
  // SWI calls from ROM and RAM regardless of cache flushes.
  rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  last_rom_translation_ptr = rom_translation_ptr;
  bios_swi_entrypoint = block_lookup_address_arm(0x8);
  rom_cache_watermark = (u32)(rom_translation_ptr - rom_translation_cache);
}

void flush_translation_cache_ram(void)
{
  /* Flushes RAM caches avoiding doing too much work (ie. wiping unused memory) */
  flush_ram_count++;
  flush_ram_total++;
  /*printf("ram flush %d (pc %x), %x to %x, %x to %x\n",
   flush_ram_count, reg[REG_PC], iwram_code_min, iwram_code_max,
   ewram_code_min, ewram_code_max);*/

  /* Phase 5g: price the flush directly instead of inferring it from a frame
   * spike.  Two clock reads on a path that is already the expensive one. */
  u32 flush_t0 = smc_prof_clock ? smc_prof_clock() : 0;

#ifdef SMC_PARTIAL_SAFE
  smc_retired_tag_count = 0;
#endif

  last_ram_translation_ptr = ram_translation_cache;
  ram_translation_ptr = ram_translation_cache;

  // Proceed to clean the SMC area if needed
  // (also try to memset as little as possible for performance)
  if (iwram_code_max) {
    if(iwram_code_max > iwram_code_min) {
      iwram_code_min &= ~15U;
      iwram_code_max = MIN(iwram_code_max + 8, 0x8000);
      memset(&iwram[iwram_code_min], 0, iwram_code_max - iwram_code_min);
      smc_flush_bytes += iwram_code_max - iwram_code_min;
    } else {
      memset(iwram, 0, 0x8000);
      smc_flush_bytes += 0x8000;
      smc_flush_wide++;
    }
  }

  if (ewram_code_max) {
    if(ewram_code_max > ewram_code_min) {
      ewram_code_min &= ~15U;
      ewram_code_max = MIN(ewram_code_max + 8, 0x40000);
      memset(&ewram[0x40000 + ewram_code_min], 0, ewram_code_max - ewram_code_min);
      smc_flush_bytes += ewram_code_max - ewram_code_min;
    } else {
      memset(&ewram[0x40000], 0, 0x40000);
      smc_flush_bytes += 0x40000;
      smc_flush_wide++;
    }
  }

  if (smc_prof_clock)
    smc_flush_us += smc_prof_clock() - flush_t0;

  iwram_code_min = ~0U;
  iwram_code_max =  0U;
  ewram_code_min = ~0U;
  ewram_code_max =  0U;
  ram_block_tag = INITIAL_TOP_TAG;
}

/* Self-modifying-code entry points: identical behaviour to
 * flush_translation_cache_ram(), separate counters.  The cache-exhaustion
 * path in translate_block_{arm,thumb} calls the plain one directly after
 * bumping flush_ram_full; the dynarec stubs call one of these two.  Splitting
 * them tells a "the JIT cache is too small" problem (a bigger cache is the
 * lever) apart from a "this game keeps writing over its own code" problem
 * (only finer-grained invalidation is), and then tells the two SMC *sources*
 * apart, which want different mitigations:
 *   _smc  a single CPU store from translated code landed on a tagged
 *         halfword (smc_write in the store stubs).
 *   _dma  a DMA transfer wrote into tagged IWRAM/EWRAM.  CPU_ALERT_SMC is
 *         raised ONLY by dma_write_iwram/dma_write_ewram (gba_memory.c), so
 *         the write_io_epilogue path is exactly and only the DMA case.  A
 *         DMA knows its whole destination range up front, so this is the
 *         source a range-invalidate could actually address. */
#ifdef SMC_PARTIAL_SAFE
/* Selective SMC invalidation used by the current hardware experiment.
 * Tags identify block starts, while per-mode end addresses record exact
 * source ranges. */
void ramtag_note_extent(u32 start_pc, u32 end_pc, u32 thumb)
{
  u16 *tagp;
  u32 off;

  if (!smc_partial_active)
    return;

  if (start_pc >= 0x03000000 && start_pc < 0x04000000) {
    tagp = (u16 *)iwram;
    off = (start_pc & 0x7FFF) >> 1;
  } else if (start_pc >= 0x02000000 && start_pc < 0x03000000) {
    tagp = (u16 *)(ewram + 0x40000);
    off = (start_pc & 0x3FFFF) >> 1;
  } else {
    return;
  }

  if (VALID_TAG(tagp[off])) {
    ramtag_type *te = get_ram_tag(tagp[off]);
    if (thumb)
      te->blk_end_thumb = end_pc;
    else
      te->blk_end_arm = end_pc;
  }
}

/* Retire every translation whose source overlaps [low, high). Cross-block
 * branches into RAM are indirect under this flag, so clearing an offset
 * cannot leave a generated jump pointing at the retired cache entry. */
static void flush_translation_cache_ram_range(u32 low, u32 high)
{
  u16 *tagp;
  u32 base, bytes, lo_off, hi_off, first, last, i, target_slot, domain_end;

  if (high <= low || (low >> 24) != ((high - 1) >> 24)) {
    flush_translation_cache_ram();
    return;
  }

  if ((low >> 24) == 3) {
    tagp = (u16 *)iwram;
    base = 0x03000000;
    bytes = 0x8000;
  } else if ((low >> 24) == 2) {
    tagp = (u16 *)(ewram + 0x40000);
    base = 0x02000000;
    bytes = 0x40000;
  } else {
    flush_translation_cache_ram();
    return;
  }

  lo_off = low - base;
  hi_off = high - base;
  if (lo_off >= bytes || hi_off > bytes) {
    flush_translation_cache_ram();
    return;
  }

  /* The first STM in SoundMainRAM is followed by three more stores that
   * patch the same generated mixer block. A full flush naturally clears all
   * of their tags on the first trap. Preserve that atomic behavior by
   * extending retirement through the gated block beginning at the reported
   * final word; later stores in the batch then see no live tags. */
  target_slot = (hi_off - 4) >> 1;
  if (!VALID_TAG(tagp[target_slot])) {
    flush_translation_cache_ram();
    return;
  }
  {
    ramtag_type *target = get_ram_tag(tagp[target_slot]);
    domain_end = target->blk_end_arm;
    if (target->blk_end_thumb > domain_end)
      domain_end = target->blk_end_thumb;
  }
  if (domain_end <= high || domain_end > base + bytes) {
    flush_translation_cache_ram();
    return;
  }
  high = domain_end;
  hi_off = high - base;

  /* ARM is the wider ISA, so this is the hard maximum source span for
   * either mode. Exact extents filter unrelated starts inside the window. */
  first = (lo_off > MAX_BLOCK_SIZE * 4)
        ? (lo_off - MAX_BLOCK_SIZE * 4) >> 1 : 0;
  last = (hi_off + 1) >> 1;
  if (last > (bytes >> 1))
    last = bytes >> 1;

  /* Validate the complete retirement set before changing any entry. */
  for (i = first; i < last; i++) {
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      u32 start = base + (i << 1);

      if ((SMC_RAM_LIVE_OFFSET(te, arm) && (!te->blk_end_arm ||
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
           (start < high && te->blk_end_arm > low &&
            te->code_bytes_arm < 8) ||
#endif
           0)) ||
          (SMC_RAM_LIVE_OFFSET(te, thumb) && (!te->blk_end_thumb ||
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
           (start < high && te->blk_end_thumb > low &&
            te->code_bytes_thumb < 8) ||
#endif
           0))) {
        flush_translation_cache_ram();
        return;
      }
    }
  }

  for (i = first; i < last; i++) {
    if (VALID_TAG(tagp[i])) {
      ramtag_type *te = get_ram_tag(tagp[i]);
      u32 start = base + (i << 1);

      if (SMC_RAM_LIVE_OFFSET(te, arm) &&
          start < high && te->blk_end_arm > low) {
#ifdef SMC_PARTIAL_STABLE_THUNK
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, arm), start, 0);
        te->code_bytes_arm = 0;
#else
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
        u8 *entry = &ram_translation_cache[te->offset_arm];
        u8 *translation_ptr = entry - 16;
        generate_load_imm(reg_a0, start);
        mips_emit_j(mips_absolute_offset(mips_indirect_branch_arm));
        mips_emit_nop();
        generate_branch_patch_unconditional(entry, entry - 16);
        address32(entry, 4) = 0;
        platform_cache_sync(entry - 16, entry + 8);
#endif
        te->offset_arm = 0;
#endif
        flush_ram_partial++;
      }
      if (SMC_RAM_LIVE_OFFSET(te, thumb) &&
          start < high && te->blk_end_thumb > low) {
#ifdef SMC_PARTIAL_STABLE_THUNK
        smc_stable_thunk_dispatch(SMC_RAM_PUBLIC_ENTRY(te, thumb), start, 1);
        te->code_bytes_thumb = 0;
#else
#ifndef SMC_PARTIAL_SAFE_NO_TRAMP
        u8 *entry = &ram_translation_cache[te->offset_thumb];
        u8 *translation_ptr = entry - 16;
        generate_load_imm(reg_a0, start);
        mips_emit_j(mips_absolute_offset(mips_indirect_branch_thumb));
        mips_emit_nop();
        generate_branch_patch_unconditional(entry, entry - 16);
        address32(entry, 4) = 0;
        platform_cache_sync(entry - 16, entry + 8);
#endif
        te->offset_thumb = 0;
#endif
        flush_ram_partial++;
      }
    }
  }


  /* Suppress the other stores in this patch iteration, as a full flush does,
   * but keep the trapping STM's FINAL word tagged. Only that highest-address
   * store uses execute_store_u32 and performs an SMC check; retaining the low
   * word instead lets a later channel iteration escape when the generated
   * target has not run yet to reconstruct its tags. The other three STMs in
   * this iteration target different final words and remain atomic with it. */
  {
    u16 trigger_tag = tagp[target_slot];
    for (i = lo_off >> 1; i < (hi_off >> 1); i++)
      if (VALID_TAG(tagp[i]) &&
          !smc_remember_retired_tag(base + (i << 1), tagp[i])) {
        flush_translation_cache_ram();
        return;
      }
    memset(&tagp[lo_off >> 1], 0, hi_off - lo_off);
    tagp[target_slot] = trigger_tag;
  }
}

#elif defined(SMC_PARTIAL)
/* Record a freshly scanned block's source extent against its tag.  Called
 * from smc_blk_note_block() the moment scan_block settles block_end_pc — and
 * crucially BEFORE the external-exit resolution that can recursively
 * translate other blocks, so no nested translation can clobber it.  The tag
 * itself is already allocated: block_lookup_translate() does that before it
 * ever calls translate_block. */
u32 smc_max_blk_len = 64;   /* longest RAM block seen, bytes (bounds the scan) */

/* UNCONDITIONAL-BRANCH BARRIERS — one bit per halfword.
 *
 * TempGBA's insight (cpu.c partial_clear_metadata): no translated block can
 * span an unconditional branch, because nothing falls through one.  So every
 * block that could possibly cover an address lies between the nearest
 * barriers around it — a bound that comes from code GEOMETRY and therefore
 * cannot be invalidated by our own bookkeeping, which is what defeated every
 * tag-adjacency scheme (v1-v6).
 *
 * gpSP already knows where these are: scan_block reports SMC_SCAN_UNCOND
 * when it stops on one.  It simply never recorded the position.
 * IWRAM 32 KiB -> 2 KiB of bits, EWRAM 256 KiB -> 16 KiB. */
static u8 uncond_iw[0x8000 >> 4];
static u8 uncond_ew[0x40000 >> 4];

static u8 *uncond_map(u32 gba_addr, u32 *hw_off, u32 *hw_lim)
{
  if (gba_addr >= 0x03000000) {
    *hw_off = (gba_addr & 0x7FFF) >> 1;  *hw_lim = 0x8000 >> 1;
    return uncond_iw;
  }
  *hw_off = (gba_addr & 0x3FFFF) >> 1;   *hw_lim = 0x40000 >> 1;
  return uncond_ew;
}

void ramtag_note_extent(u32 start_pc, u32 end_pc, u32 thumb)
{
  u16 *tagp;
  u32  off;

  if (end_pc > start_pc && (end_pc - start_pc) > smc_max_blk_len)
    smc_max_blk_len = end_pc - start_pc;

  if (start_pc >= 0x03000000 && start_pc < 0x04000000) {
    tagp = (u16 *)iwram;   off = (start_pc & 0x7FFF) >> 1;
  } else if (start_pc >= 0x02000000 && start_pc < 0x03000000) {
    tagp = (u16 *)(ewram + 0x40000); off = (start_pc & 0x3FFFF) >> 1;
  } else {
    return;                              /* ROM block: no RAM tag to annotate */
  }

  if (VALID_TAG(tagp[off])) {
    ramtag_type *te = get_ram_tag(tagp[off]);
    te->blk_start = start_pc;
    te->blk_end   = end_pc;
  }
  (void)thumb;
}

/* Record "no block spans this point" at a block end that stopped on an
 * unconditional branch.  Called only for SMC_SCAN_UNCOND. */
void ramtag_note_barrier(u32 end_pc)
{
  u32 h, lim;
  u8 *map;
  if (end_pc < 0x02000000 || end_pc >= 0x04000000)
    return;
  map = uncond_map(end_pc, &h, &lim);
  if (h < lim)
    map[h >> 3] |= (u8)(1u << (h & 7));
}

/* Partial RAM invalidation (experiment).
 *
 * Measured on Pokemon Unbound: one self-modifying ARM block in IWRAM
 * (030061e4-03006358) patches four words of its own body ~1450x/second, and
 * every one of those writes wipes the ENTIRE RAM translation cache.  The
 * telemetry priced the collateral at ~5 block re-translations per write
 * (xlat ~37000 for ~7040 writes), and dynarec compilation then eats 40-61%
 * of the frame.
 *
 * The tag array doubles as a block map: 0 = no code, CODE_TAG_BLOCK16
 * (0x0101) = an instruction inside a block, anything greater = a block START
 * whose value indexes the ramtag table.  So the block covering an address is
 * found by walking back to the nearest block-start tag and forward to the
 * end of the tagged run.  Clearing just those tags retires exactly one block;
 * everything else keeps its translation.
 *
 * NOTE ON SAFETY — this is why it is behind a flag and lives in the lab.
 * translate_block patches DIRECT branches between blocks
 * (generate_branch_patch_unconditional at the end of translate_block_arm).
 * If another block already holds a patched jump into the block we retire, it
 * will keep jumping to the stale translation.  The deterministic frame-dump
 * oracle in the harness is what decides whether that happens in practice for
 * this game: identical dump = safe here, different dump = this needs
 * back-reference bookkeeping before it can ever ship.
 *
 * ram_translation_ptr is deliberately NOT rewound — the retired code is
 * simply abandoned.  The cache-exhaustion path in translate_block already
 * calls the full flush when it fills, so the leak is self-limiting. */
static void flush_translation_cache_ram_block(u32 gba_addr)
{
  u16 *tagbase, *tagp;
  u32 off, lim;

  if (gba_addr >= 0x03000000) {          /* IWRAM: tags live 32 KiB below */
    tagbase = (u16 *)(iwram);
    off     = (gba_addr & 0x7FFF) >> 1;
    lim     = 0x8000 >> 1;
  } else {                               /* EWRAM: tags live at +0x40000 */
    tagbase = (u16 *)(ewram + 0x40000);
    off     = (gba_addr & 0x3FFFF) >> 1;
    lim     = 0x40000 >> 1;
  }

  tagp = tagbase;
  if (!tagp[off])                        /* not code after all — nothing to do */
    return;

  /* WHY A WINDOW AND NOT JUST "THE" BLOCK.
   *
   * v1 walked back to the nearest block start and retired that one block.
   * It ran Unbound at 59.9 fps with a byte-identical video frame — and
   * silently corrupted audio (hash e9c60c3e vs baseline 358b532f, proven
   * deterministic by a repeat run).  A trampoline at the retired entry point
   * changed nothing, which ruled out stale jumps.
   *
   * The real flaw: gpSP starts a block at every branch target, so SEVERAL
   * blocks can span one address.  A block starting further back can reach
   * across the written word, and its own start tag halts the backward walk,
   * so v1 never found it.  That block stays live holding stale code — which
   * is why the M4A sound driver kept playing outdated mixer code while
   * graphics were unaffected.  The full flush was correct precisely because
   * it killed every block regardless of overlap.
   *
   * A block is at most MAX_BLOCK_SIZE (1024) instructions = 4 KiB, so any
   * block covering this address must START within 4 KiB behind it.  Retire
   * every block starting in that window.  Over-invalidation is safe (it only
   * costs re-translation); under-invalidation is the bug above. */
  {
    /* v2 used a flat 4 KiB window (MAX_BLOCK_SIZE) and was CORRECT but slow:
     * 26.9 fps, worse than the 29.4 baseline, because it retired every block
     * in the window whether or not it reached the write.
     *
     * Tighter and still exact: scan_block tags EVERY instruction slot of a
     * block, so a block that spans this address has an unbroken run of tags
     * from its own start all the way through it.  Walking back only while the
     * tags stay non-zero therefore reaches every block that can possibly
     * cover the write, and stops dead at the first untagged byte — no block
     * spans a gap.  A fixed window can only be larger than this, never more
     * correct. */
    /* Window bounded by the LONGEST BLOCK SEEN SO FAR, not by tag adjacency.
     *
     * Each earlier attempt failed in its own way and this combines what they
     * proved.  Following the tag run (v3/v5) is fragile: retiring punches
     * holes in it (v4 -> wrong audio) and never clearing lets the run grow
     * until the scan covers all of IWRAM (v5 -> the emulator stalled before
     * its first heartbeat).  A flat 4 KiB window (v2) was correct but retired
     * everything in it and ran slower than the full flush.
     *
     * Any block spanning this address must START no earlier than
     * smc_max_blk_len bytes behind it — that is a hard bound, independent of
     * the tag map's shape, so fragmentation cannot defeat it and the scan can
     * never run away.  Measured blocks here are ~400 bytes, far below the
     * 4 KiB MAX_BLOCK_SIZE ceiling, so the window stays small.  Coverage is
     * then tested EXACTLY against each candidate's recorded extent.
     * Tags are never cleared: they mean "code lives here", which stays true,
     * and clearing them would blind the SMC check to future writes. */
    u32 bh, blim;
    u8 *bmap = uncond_map(gba_addr, &bh, &blim);
    u32 cap  = (smc_max_blk_len >> 1) + 2;      /* hard ceiling, in slots */
    u32 lo = off, hi = off, i, steps = 0;

    /* This write may have edited the very instruction a barrier stands on,
     * so drop barriers across the written word before trusting them. */
    if (bh < blim) {
      bmap[bh >> 3]       &= (u8)~(1u << (bh & 7));
      if (bh + 1 < blim)
        bmap[(bh+1) >> 3] &= (u8)~(1u << ((bh+1) & 7));
    }

    /* Walk back to the nearest barrier — provably past every block that can
     * reach this address.  The cap is a safety valve: if no barrier turns up
     * within the longest block ever seen, fall back to the full flush rather
     * than scan further.  Correctness never depends on the walk being short. */
    while (lo > 0 && steps < cap &&
           !(bmap[(lo - 1) >> 3] & (1u << ((lo - 1) & 7)))) {
      lo--; steps++;
    }
    if (steps >= cap) {
      flush_translation_cache_ram();
      return;
    }

    while (hi + 1 < lim && tagp[hi + 1] == CODE_TAG_BLOCK16)
      hi++;                              /* forward to the end of this block */

    for (i = lo; i <= hi; i++) {
      if (VALID_TAG(tagp[i])) {
        ramtag_type *te = get_ram_tag(tagp[i]);
        /* Retire only blocks that genuinely span the written address.  With
         * a recorded extent this is exact: no missed overlaps (v1's audio
         * corruption) and no needless retirements (v2 being slower than the
         * full flush).  Blocks with no extent recorded are retired anyway —
         * unknown means assume guilty. */
        if (!te->blk_end ||
            (gba_addr >= te->blk_start && gba_addr < te->blk_end)) {
          /* Retire by clearing the CACHE OFFSETS only — never the tags.
           *
           * v4 zeroed the retired block's tags and was still wrong (a third
           * audio hash, 50bdf44f).  Reason: the walk-back above relies on an
           * unbroken run of tags from a block's start through the written
           * address, and zeroing tags punches holes in runs that OTHER,
           * overlapping blocks still depend on — so the next write stops
           * early at the hole and misses blocks starting before it.  Our own
           * bookkeeping was destroying the map we navigate by.
           *
           * The tag only needs to mean "code lives at this halfword", which
           * stays true.  block_lookup_translate reuses the existing tag when
           * VALID_TAG holds and re-translates whenever the offset is 0, so
           * clearing the offset alone is a complete retirement — and the tag
           * map stays contiguous forever. */
          /* LEAVE A TRAMPOLINE AT THE OLD ENTRY POINT.
           *
           * Retiring by clearing offsets is enough only while nothing holds a
           * patched direct jump into this block.  Translation gates break that
           * assumption on purpose: they make the self-modified addresses into
           * block STARTS, and block starts are the only thing
           * generate_branch_patch_unconditional ever targets.  So the gates
           * manufacture inbound jumps pointing exactly at the words the sound
           * driver rewrites — and a retired block still gets entered through
           * them, running the stale translation.  Audible as music that plays
           * correctly, screeches, recovers, screeches: dispatcher entry is
           * fine, stale-jump entry is garbage.
           *
           * Overwriting the entry with a jump to the block-lookup dispatcher
           * makes every stale jump land somewhere that re-translates the
           * modified code.  (Tried once before against a DIFFERENT bug —
           * missed overlapping blocks — where it correctly changed nothing.) */
          /* SIZE GUARD.  The trampoline is 4 instructions = 16 bytes.  A block
           * whose translation is shorter than that would have the trampoline
           * spill into whatever was emitted next — and translation gates make
           * short blocks common by design, because they deliberately split at
           * the self-modified words.  Corrupting a neighbouring block is
           * exactly the kind of damage that shows up as intermittently
           * mangled audio, so refuse to write one that does not fit and take
           * the safe full flush instead. */
          if (te->blk_start && te->code_bytes >= 16) {
            unsigned k;
            for (k = 0; k < 2; k++) {
              u32 coff = k ? te->offset_thumb : te->offset_arm;
              if (coff) {
                u8 *entry = &ram_translation_cache[coff];
                u8 *translation_ptr = entry;
                mips_emit_lui(reg_a0, (te->blk_start >> 16) & 0xFFFF);
                mips_emit_ori(reg_a0, reg_a0, te->blk_start & 0xFFFF);
                mips_emit_j(((u32)(k ? &mips_indirect_branch_thumb
                                     : &mips_indirect_branch_arm)) >> 2);
                mips_emit_nop();
                platform_cache_sync(entry, translation_ptr);
              }
            }
          }
          te->offset_arm   = 0;
          te->offset_thumb = 0;
          flush_ram_partial++;
        }
      }
    }

    /* MAKE THE RE-TRANSLATION CHEAP, not just rare.
     *
     * Measured: correct invalidation alone gets 31.5 fps vs 29.4 baseline —
     * only 7%.  Retiring the right blocks was never the expensive part; the
     * cost is recompiling ONE 372-byte self-modifying routine ~1450 times a
     * second.  The 59.9 fps variants were fast only because they skipped
     * that necessary work.
     *
     * gpSP already supports translation gates (scan_block stops when
     * block_end_pc hits one) but nothing ever populates the list.  Adding a
     * gate at the written address splits future blocks there, so the word
     * being patched sits on a block boundary and only a SMALL block has to be
     * rebuilt each time instead of the whole routine.  This is what TempGBA's
     * partial_flush_ram_stub does after its partial clear.
     *
     * 8 slots, and Unbound's storm is 4 addresses, so they fit. */
    /* (gate insertion now lives in flush_translation_cache_ram_smc, behind
     * SMC_GATES, so gates and partial invalidation can be A/B'd separately) */
    return;
  }
#if 0  /* v1, kept for the record — see the comment above for why it is wrong */
  while (off > 0 && tagp[off] == CODE_TAG_BLOCK16)
    off--;

  if (!VALID_TAG(tagp[off])) {
    flush_translation_cache_ram();
    return;
  }

  {
    ramtag_type *trentry = get_ram_tag(tagp[off]);
    /* THE CORRECTNESS PIECE.  Clearing the tag alone is NOT enough: other
     * blocks hold patched DIRECT jumps into this block's entry point, and
     * they would keep running the stale translation.  Measured consequence:
     * Unbound's video stayed byte-identical while its audio hash changed
     * (e9c60c3e vs 358b532f) because the M4A sound driver in IWRAM is
     * exactly the self-modifying code being retired.
     *
     * So leave a TRAMPOLINE at the old entry point instead of abandoning it.
     * gpSP only ever patches branches to block STARTS, so every stale jump
     * lands here and gets re-dispatched through the normal lookup, which
     * re-translates the modified code.  4 instructions, written once per
     * retirement. */
    u32 blk_pc = ((gba_addr >= 0x03000000) ? 0x03000000 : 0x02000000)
               + (off << 1);
    unsigned k;
    for (k = 0; k < 2; k++)
    {
      u32 coff = k ? trentry->offset_thumb : trentry->offset_arm;
      if (coff)
      {
        u8 *entry = &ram_translation_cache[coff];
        u8 *translation_ptr = entry;
        mips_emit_lui(reg_a0, (blk_pc >> 16) & 0xFFFF);
        mips_emit_ori(reg_a0, reg_a0, blk_pc & 0xFFFF);
        mips_emit_j(((u32)(k ? &mips_indirect_branch_thumb
                             : &mips_indirect_branch_arm)) >> 2);
        mips_emit_nop();
        platform_cache_sync(entry, translation_ptr);
      }
    }
    trentry->offset_arm   = 0;
    trentry->offset_thumb = 0;
  }

  tagp[off++] = 0;                       /* retire the start ... */
  while (off < lim && tagp[off] == CODE_TAG_BLOCK16)
    tagp[off++] = 0;                     /* ... and its instruction run */

  flush_ram_partial++;
#endif  /* v1 */
}
#endif

#ifdef SMC_GATES
/* Split future blocks at the self-modified address, so each rebuild is a small
 * block instead of the whole routine.  Hoisted OUT of the partial-invalidation
 * path so the two can be tested independently: gates alone change block
 * boundaries (and therefore when timers/DMA get serviced), which is the
 * remaining suspect for Unbound's mangled audio now that stale jumps are
 * ruled out. */
/* WHICH ADDRESSES GET A GATE.
 *
 * A gate is not free.  It ends a block with an indirect branch, so every
 * pass through it is a dispatcher lookup; its only benefit is that a write
 * there retires a small block instead of a large one.  Measured per gate
 * (SMC hits vs lookups at that PC), two rules came out of it:
 *
 *  1. ONE GATE PER PATCHED REGION.  A self-modifying routine patches several
 *     words a few bytes apart -- the M4A sound mixer rewrites 4 words within
 *     56 bytes, in two copies of its loop 152 bytes apart.  The first gate
 *     already makes every later write in the region retire a short block;
 *     further gates there saved no translation at all and each added ~25k
 *     lookups a second, because they sit in the mixer's inner loop.  So a
 *     write within SMC_GATE_CLUSTER bytes after an existing gate counts as a
 *     hit on that gate.  128 keeps the mixer's two loop copies apart.
 *
 *  2. REPLACE GATES THAT HAVE GONE QUIET.  The 8 slots fill with whatever is
 *     written first.  Pokemon Heart & Soul spends all of them at boot on
 *     stack and copy writes that never recur, so its music engine never got
 *     one: 457 KB/s re-translated, and a PSP-1000 dropped to 37 fps whenever
 *     music played.  Once the table is full, a gate not hit for
 *     SMC_GATE_IDLE_FRAMES is replaced.  Gates from the game table
 *     (gba_over.h) carry no stamp and are never replaced.
 *
 * Hardware A/B, Heart & Soul on a PSP-1000 with music on: 37-60 fps -> a
 * locked 60; re-translation 457 -> 48 KB/s, full flushes 8.0 -> 1.7/s.
 * PPSSPP, Unbound: lookups -49%, translation unchanged, core time -10%.
 * Unbound never fills the table, so rule 2 never fires there. */
/* Overridable from the root make as CLUSTER_BYTES=N.  Sized to the GAME's
 * layout, not picked for roundness: H&S's M4A mixer patches two loop copies
 * 152 bytes apart, so a window under that gates each copy separately while
 * anything over it merges them and leaves the second ungated.  64 was too
 * small (slots wasted inside one region: 105k dispatcher lookups vs 59k). */
#ifndef SMC_GATE_CLUSTER
#define SMC_GATE_CLUSTER      128
#endif
#define SMC_GATE_IDLE_FRAMES  60

/* frame_counter + 1 at the gate's last hit; 0 = loaded from the game table */
static u32 smc_gate_last_hit[MAX_TRANSLATION_GATES];
#if defined(SMC_GATES) && !defined(SMC_GATES_SIMPLE) && !defined(SMC_GATES_CLUSTER)
static u32 smc_gate_prev_now;   /* detects savestate clock rewind */
#endif

#ifdef SMC_GATES_RANKED
static void smc_cand_forget(void);   /* defined with the candidate table below */
#endif

/* FORGET EVERY LEARNED THING, not just the hit stamps.
 *
 * The ranked candidate table is where the gate rule keeps its evidence, and it
 * was the one piece nothing ever cleared: gba_memory.c zeroes
 * translation_gate_targets at ROM load and calls this, and this only reset the
 * ages.  So the 32 candidates -- addresses, write counts, and the last VALUE
 * seen at each address -- survived from one game into the next.
 *
 * That is not merely untidy, it inverts the promotion rule.  Unbound and Heart
 * & Soul are both M4A engines, so both copy a sound driver into nearly the same
 * IWRAM addresses; a candidate Unbound had already driven past
 * SMC_GATE_MIN_SAMPLE is therefore still sitting there, saturated, when Heart &
 * Soul writes the same address for the FIRST time.  smc_gate_earned() finds the
 * stale row, compares the new word against a value left over from the other
 * game (so it counts as changed), and promotes on that single observation --
 * the 64-sample evidence requirement silently becomes a 1-sample one, and the
 * address it gates was never shown to be self-modifying code in THIS game.
 *
 * Invariant, stated once: learned dynarec state may not outlive the evidence
 * that produced it.  Called from ROM load, from init_dynarec_caches (emulator
 * reset) and from flush_dynarec_caches (savestate load, cheat install,
 * libretro load/reset), which is every boundary where that evidence dies. */
void smc_gates_reset(void)
{
  memset(smc_gate_last_hit, 0, sizeof(smc_gate_last_hit));
#if defined(SMC_GATES) && !defined(SMC_GATES_SIMPLE) && !defined(SMC_GATES_CLUSTER)
  /* Savestate-rewind re-anchor: a stale stamp against a fresh frame_counter is
   * the underflow that made eviction fire on every write. */
  smc_gate_prev_now = 0;
#endif
#ifdef SMC_GATES_RANKED
  smc_cand_forget();
#endif
  SMC_GATE_MAP_REBUILD();
}

/* SMC_GATES_CODED: only gate an address that is ALREADY the start of a
 * translated block.
 *
 * Gates are placed at smc_last_write_addr -- a DATA write address -- but a
 * gate is then used as a CODE entry: it forces blocks to end there, so the
 * next block STARTS there.  Nothing guarantees a write address is an
 * instruction boundary, and mid-copy it is not even finished code.  Scanning
 * from such an address yields nonsense: one PPSSPP battle logged 223 bad
 * branch targets (into OAM/VRAM/SRAM) and every one came from the block at
 * the gated address 0x03001404.  Most are never executed; when one is, the
 * dispatcher jumps to it and the console dies.
 *
 * A VALID_TAG at the address means a block was translated starting exactly
 * there, which is proof it is a real instruction boundary.  Gating it still
 * pays: it stops OTHER blocks from spanning the address, which is the
 * overlap the one-slot-per-halfword tag map cannot represent. */
/* SMC_GATES_SNAP: gate the START OF THE BLOCK containing the write, not the
 * write address itself.
 *
 * Gates are harvested from smc_last_write_addr -- a DATA address -- but a
 * gate is consumed as a CODE entry: it forces blocks to end there, so the
 * next block STARTS there.  Nothing makes a data-write address an
 * instruction boundary, and mid-copy it is not finished code either.  That
 * is the whole crash: one battle logged 223 garbage branch targets, every
 * one from the block at the gated address 0x03001404.
 *
 * A VALID_TAG exists at an address ONLY because allocate_tag_* ran with that
 * exact pc, i.e. a block was translated starting there -- so it is a proven
 * instruction boundary, in the right mode.  Walk back over the interior
 * (CODE_TAG_BLOCK16) run to find it.
 *
 * This is what SMC_GATES_CODED should have been.  That one demanded the
 * write address ITSELF be a block start, which rejected 0x0300168c -- the
 * address behind 97.8% of all flushes, interior to its block -- and cost
 * 59.9 -> 46-58 fps.  Snapping keeps that gate, just anchored to the head of
 * its block, so scans still stop before the self-modifying region.
 *
 * Bonus: every write inside one block snaps to the same gate, so the table
 * stops churning through near-duplicates.  That is SMC_GATE_CLUSTER grouping
 * done by real block boundaries instead of a fixed byte window. */
/* SMC_GATES_SNAPF: snap FORWARD to the first proven instruction boundary at
 * or after the write address.
 *
 * A gate address has to satisfy TWO things, and SMC_GATES_SNAP only got one:
 *   (1) validity  -- it is used as a code entry, so it must be a real
 *                    instruction boundary, else the block scanned from it is
 *                    garbage (223 bad jumps, all from gated 0x03001404);
 *   (2) isolation -- it must be at/after the write, so the previous block
 *                    ENDS before the self-modifying region instead of
 *                    spanning it.  That is the entire speed mechanism.
 * SNAP walked BACKWARDS to the block head: valid, but the block starting at
 * that head still covers the SMC region, so isolation was lost -- measured
 * 1,428,000 flushes and 31-48 fps, against 116,000 and a locked 59.9 for
 * add-only.  Snapping forward keeps the anchor AND the isolation.
 *
 * Stride is 4, unconditionally: start+4k is a valid boundary for ARM (whose
 * instructions are 4 bytes) and for Thumb (2 bytes, so every even offset is
 * one), which sidesteps needing the block mode.  A Thumb block that starts on
 * a BL second half is safe -- translate_block_thumb inits opcode = 0, so
 * last_opcode is 0 on the first instruction and thumb_branch_target takes the
 * no_direct_branch path instead of computing a target from stale bits. */
#ifdef SMC_GATES_SNAPF
static u32 smc_gate_snapf(u32 gba_addr)
{
  u8 *base; u32 off, region, mask, k, delta;
  if (gba_addr >= 0x3000000) {
    base = iwram;           mask = 0x7FFF;  region = 0x3000000;
  } else {
    base = ewram + 0x40000; mask = 0x3FFFF; region = 0x2000000;
  }
  off = (gba_addr & mask) & ~1u;
  for (k = 0; k < 512; k++) {              /* find the enclosing block head */
    u16 t = *(u16 *)(base + off);
    if (VALID_TAG(t)) break;                /* proven instruction boundary */
    if (t != CODE_TAG_BLOCK16) return 0;    /* data -- nothing to gate */
    if (off < 2) return 0;
    off -= 2;
  }
  if (k == 512) return 0;                   /* gave up; do not guess */
  delta = ((gba_addr & mask) & ~1u) - off;  /* write, relative to the head */
  return region + off + ((delta + 3u) & ~3u);
}
#define SMC_GATE_PICK(a) smc_gate_snapf(a)
#elif defined(SMC_GATES_SNAP)

#define SMC_GATE_SNAP_MAX 512        /* halfword slots (1 KB) before giving up */
static u32 smc_gate_snap(u32 gba_addr)
{
  u8 *base; u32 off, region, k;
  if (gba_addr >= 0x3000000) {
    base = iwram;            off = gba_addr & 0x7FFF;  region = 0x3000000;
  } else {
    base = ewram + 0x40000;  off = gba_addr & 0x3FFFF; region = 0x2000000;
  }
  off &= ~1u;                          /* tags are one u16 per halfword */
  for (k = 0; k < SMC_GATE_SNAP_MAX; k++) {
    u16 t = *(u16 *)(base + off);
    if (VALID_TAG(t))            return region + off;  /* proven block start */
    if (t != CODE_TAG_BLOCK16)   return 0;   /* data -- nothing to gate */
    if (off < 2)                 return 0;
    off -= 2;
  }
  return 0;                              /* run too long; do not guess */
}
#define SMC_GATE_PICK(a) smc_gate_snap(a)
#else
#define SMC_GATE_PICK(a) ((a) & ~3u)
#endif

#ifdef SMC_GATES_CODED
static int smc_gate_addr_is_block_start(u32 gba_addr)
{
  const u16 *tagp = (gba_addr >= 0x3000000)
    ? (const u16 *)(iwram + (gba_addr & 0x7FFF))
    : (const u16 *)(ewram + (gba_addr & 0x3FFFF) + 0x40000);
  u16 t = *tagp;
  return VALID_TAG(t);
}
#define SMC_GATE_REJECT(gpc) (!smc_gate_addr_is_block_start(gpc))
#else
#define SMC_GATE_REJECT(gpc) (0)
#endif

/* SMC_GATES_RANKED: promote an address to a gate only once it has proven
 * itself HOT.  Both shipped rules pick gates blind -- add-only takes the
 * first eight addresses it ever sees, eviction cycles round-robin -- and
 * neither asks how much a gate would actually buy.
 *
 * Measured on the H&S rival battle, over one 8000-flush window:
 *
 *   0300168c   7723 writes   real code, value CHANGES (62953 vs 120 same)
 *              a block starts here and runs 030016 8c..030017c4
 *              -> the gate that matters; this is 97.8% of all flushes
 *
 *   03001404     67 writes   DATA inside a code block, value IDEMPOTENT
 *              (66 same, 1 changed).  No block starts here; TWO overlapping
 *              blocks span it (030013e8..03001414 and 030013f8..03001414).
 *              Gating it forces a block to start ON THE DATA WORD, which
 *              then scans garbage -- 223 bad branch targets in one battle,
 *              and the crash when one of them is executed.
 *
 * The two differ by ~115x in hit count, so ranking separates them cleanly:
 * gate the hot one, never spend a slot on the cold one.  Add-only semantics
 * are kept -- once promoted a gate is never moved -- so the table cannot
 * churn onto a bad address later, which is what made eviction unsafe. */
#ifdef SMC_GATES_RANKED
#ifndef SMC_GATE_RATIO
#define SMC_GATE_RATIO 4            /* within 1/4 of the hottest */
#endif
#define SMC_GATE_MIN_SAMPLE 64     /* writes before judging an address */
/* WITHDRAWN: A STALENESS RULE ON THE CANDIDATE ROWS.
 *
 * A gap over ~60 frames restarted a row's statistics, the reasoning being that
 * an address hot in one role (a boot copy destination, the previous
 * soundtrack's mixer body) should not promote on one write when the game later
 * uses it in another.
 *
 * REMOVED after a hardware session on 2026-09-18 showed degraded performance.
 * The rule and the thing it gates are the same order of magnitude: earning a
 * gate needs SMC_GATE_MIN_SAMPLE writes AT ONE ADDRESS, and if that takes
 * longer than the window the row restarts forever and the gate is NEVER
 * earned -- which is the documented 457 KB/s re-translation, 37 fps regime.
 * I never measured writes-per-frame at a single address before choosing 60,
 * so the window may well have been inside the accumulation time.
 *
 * It was also the only change on this branch with no demonstrated defect
 * behind it -- unlike the cross-ROM leak, the role-change case was a
 * hypothesis.  And it is defence in depth rather than the guard: with the
 * containment fix, a wrongly promoted gate is no longer fatal, it is a
 * dispatcher lookup.  That is what makes this safe to drop.
 *
 * If it is ever wanted back, the window has to be derived from measured
 * writes-per-frame at the gate address, with a wide margin -- not chosen.
 *
 * (The kept half:
 *
 * Nothing ever aged a candidate row, so `hits` and `chg` were lifetime totals.
 * An address that was hot in one role -- a copy destination during boot, a
 * mixer body for the previous soundtrack -- keeps a saturated row for the whole
 * session.  If the game later writes that address in a DIFFERENT role, the
 * first such write finds hits already past SMC_GATE_MIN_SAMPLE and a stale
 * remembered value that almost certainly differs, so the ratio holds and the
 * address is promoted on ONE observation of its new role.  That is the same
 * inversion as the cross-ROM leak, reachable inside a single game, and it lines
 * up with the reported trigger: changing Heart & Soul's soundtrack moves which
 * IWRAM words the M4A engine patches.
 *
 * So a row measures one continuous episode.  A gap longer than this retires the
 * row's statistics AND its remembered value, and counting starts again.  The
 * mixer writes thousands of times a second while music plays, so it never sees
 * a gap; one second of silence costs it 64 writes -- about 20 ms of mixer
 * activity -- to re-earn.
 *
 * an absolute floor on `chg` was tried first and was worse than useless: at
 * SMC_GATE_MIN_SAMPLE 64 a floor of 32 is exactly what `chg * 2 >= hits`
 * already requires.) */
#define SMC_GATE_CAND 32            /* candidates tracked before promotion */
static u32 smc_cand_addr[SMC_GATE_CAND];
static u32 smc_cand_hits[SMC_GATE_CAND];
static u32 smc_cand_used;

/* Returns 1 when this address has earned a gate. */
static u32 smc_cand_val[SMC_GATE_CAND];   /* last value seen at the address */
static u32 smc_cand_chg[SMC_GATE_CAND];   /* times it actually CHANGED */

/* Gate only addresses whose writes genuinely MODIFY memory.
 *
 * Frequency was the wrong discriminator and failed twice: an absolute
 * threshold is outgrown by any cold address (crash b2c8), and a ratio to the
 * hottest candidate silently degenerates into one, because a promoted gate
 * stops counting and the max freezes (crash b3c8).  Counting only ever
 * delays the bad promotion.
 *
 * The real property is structural and measured:
 *     0300168c   120 same / 62953 changed  -> code being patched. SAFE, and
 *                a block legitimately starts here once gated.
 *     03001404    66 same /     1 changed  -> a constant being re-stored.
 *                DATA inside a code block, spanned by two overlapping
 *                blocks, no block starts here.  Gating it puts a block on a
 *                data word, which scans garbage and crashes.
 *
 * An idempotent store did not modify anything, so the address is not
 * self-modifying code in any useful sense -- it is data, and data must never
 * become a code entry point.  1 vs 62953 is a structural gap, not a tuned
 * constant.  Still add-only: promotion is permanent. */
static u32 *smc_gba_ptr(u32 addr)
{
  return (addr >= 0x3000000)
    ? (u32 *)(iwram + 0x8000 + (addr & 0x7FFF))
    : (u32 *)(ewram + (addr & 0x3FFFF));
}

static int smc_gate_earned(u32 gpc)
{
  u32 k, coldest = 0, cur = *smc_gba_ptr(gpc);
  for (k = 0; k < smc_cand_used; k++)
    if (smc_cand_addr[k] == gpc) {
      smc_cand_hits[k]++;
      if (cur != smc_cand_val[k]) smc_cand_chg[k]++;
      smc_cand_val[k] = cur;
      /* enough evidence, and the writes really do rewrite the bytes */
      return smc_cand_hits[k] >= SMC_GATE_MIN_SAMPLE &&
             smc_cand_chg[k] * 2 >= smc_cand_hits[k];
    }
  if (smc_cand_used >= SMC_GATE_CAND) {
    for (k = 1; k < SMC_GATE_CAND; k++)
      if (smc_cand_hits[k] < smc_cand_hits[coldest]) coldest = k;
  } else coldest = smc_cand_used++;
  smc_cand_addr[coldest] = gpc;
  smc_cand_hits[coldest] = 1;
  smc_cand_chg[coldest]  = 0;
  smc_cand_val[coldest]  = cur;
  return 0;
}
static void smc_cand_forget(void)
{
  memset(smc_cand_addr, 0, sizeof(smc_cand_addr));
  memset(smc_cand_hits, 0, sizeof(smc_cand_hits));
  memset(smc_cand_val,  0, sizeof(smc_cand_val));
  memset(smc_cand_chg,  0, sizeof(smc_cand_chg));
  smc_cand_used = 0;
}

#define SMC_GATE_EARNED(gpc) smc_gate_earned(gpc)
#else
#define SMC_GATE_EARNED(gpc) (1)
#endif

#ifdef SMC_GATES_CLUSTER
/* Add-only, PLUS 2.0.2's cluster grouping -- and deliberately WITHOUT its idle
 * eviction, which is the part that crashed Heart & Soul.
 *
 * 2.0.2's rule did two separable things.  Clustering declines to spend a slot
 * on a write within SMC_GATE_CLUSTER bytes of an existing gate, because extra
 * gates inside one patched region save no translation and cost a dispatcher
 * lookup on every pass (measured: lookups 196k -> 59k).  Eviction reassigns a
 * quiet gate's ADDRESS, which is what let already-translated blocks disagree
 * with the table.  Only the second one moves a gate, so clustering can be kept
 * and eviction dropped.  Once a gate is placed here it is never moved.  */
static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), g;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif
  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
      return;                       /* already gated */

  for (g = 0; g < translation_gate_targets; g++)
  {
    u32 gp = translation_gate_target_pc[g];
    if (gpc > gp && gpc - gp <= SMC_GATE_CLUSTER)
      return;                       /* same patched region -- covered already */
  }

  if (!SMC_GATE_EARNED(gpc)) return;   /* not hot enough yet */
  if (translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
  }
  /* Table full: leave every existing gate exactly where it is. */
}
#elif defined(SMC_GATES_SIMPLE)
/* The 2.0.1 gate rule: ADD ONLY.  An address either already has a gate or
 * takes a free slot; once written, a gate's address is NEVER changed.
 *
 * 2.0.2 replaced this with cluster-128 + idle eviction, which measured better
 * in PPSSPP.  But eviction means the gate table MUTATES while blocks compiled
 * against the old layout are still live, and gpSP's tag map holds one slot per
 * halfword — it cannot represent the overlapping blocks that creates.  On
 * 2026-09-12 a stock build (no gates at all) survived a full rival battle and
 * four Growls at the highest flush rate we have ever measured (~1900/s), while
 * every gated build died.  That puts the gate rule itself under suspicion, and
 * this is the variable to eliminate first: Heart & Soul has never run on the
 * simple rule, because it did not boot at all before 2.0.2.
 *
 * There is also a concrete defect in the evicting version: `now` derives from
 * frame_counter, which savestates restore, while smc_gate_last_hit[] does not.
 * Loading a state with a lower frame count underflows the idle comparison to
 * ~4.29e9, so every gate reads as infinitely idle and churns constantly. */
static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), g;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif
  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
      return;                       /* already gated — nothing to do */

  if (!SMC_GATE_EARNED(gpc)) return;   /* not hot enough yet */
  if (translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
  }
  /* Table full: leave every existing gate exactly where it is. */
}
#else
static void smc_add_gate(u32 gba_addr)
{
  u32 gpc = SMC_GATE_PICK(gba_addr), now = frame_counter + 1, g, victim;
#if defined(SMC_GATES_SNAP) || defined(SMC_GATES_SNAPF)
  if (!gpc) return;   /* no proven code entry for this write */
#endif

  /* frame_counter is RESTORED by savestates (main.c "frame-count") but
   * smc_gate_last_hit[] is not, so loading a state made earlier drags `now`
   * backwards while the hit stamps stay large.  The idle test below is an
   * unsigned subtraction, so it then underflows to ~4.29e9 -- always >=
   * SMC_GATE_IDLE_FRAMES -- and eviction fires on EVERY smc write instead of
   * on genuinely idle gates.  At ~11k smc writes a battle that churns the
   * table thousands of times, and every fresh address is another chance to
   * gate something that is not an instruction boundary.  Measured: 223 bad
   * jumps and a crash in battle 1, every run.  Re-anchor on a backwards
   * jump so eviction stays what it was meant to be. */
  if (now < smc_gate_prev_now)
    for (g = 0; g < MAX_TRANSLATION_GATES; g++)
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
  smc_gate_prev_now = now;

  if (SMC_GATE_REJECT(gpc)) return;   /* not a proven code entry */

  for (g = 0; g < translation_gate_targets; g++)
    if (translation_gate_target_pc[g] == gpc)
    {
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
      return;
    }

  for (g = 0; g < translation_gate_targets; g++)
  {
    u32 gp = translation_gate_target_pc[g];
    if (gpc > gp && gpc - gp <= SMC_GATE_CLUSTER)
    {
      if (smc_gate_last_hit[g])
        smc_gate_last_hit[g] = now;
      return;
    }
  }

  if (!SMC_GATE_EARNED(gpc)) return;   /* not hot enough yet */
  if (translation_gate_targets < MAX_TRANSLATION_GATES)
  {
    smc_gate_last_hit[translation_gate_targets] = now;
    translation_gate_target_pc[translation_gate_targets++] = gpc;
    SMC_GATE_MAP_ADD(gpc);
    return;
  }

  victim = MAX_TRANSLATION_GATES;
  for (g = 0; g < translation_gate_targets; g++)
    if (smc_gate_last_hit[g] &&
        (victim == MAX_TRANSLATION_GATES ||
         smc_gate_last_hit[g] < smc_gate_last_hit[victim]))
      victim = g;
  if (victim < MAX_TRANSLATION_GATES &&
      now - smc_gate_last_hit[victim] >= SMC_GATE_IDLE_FRAMES)
  {
    translation_gate_target_pc[victim] = gpc;
    smc_gate_last_hit[victim] = now;
    SMC_GATE_MAP_REBUILD();
  }
}
#endif  /* SMC_GATES_SIMPLE */
#endif

#ifdef SMC_PARTIAL_SAFE
/* Describe the complete write that led to the SMC trap. ARM STM emits
 * unchecked stores for every word except the highest address.
 * A non-writeback STM is still safe to retire selectively once we decode the
 * register count and cover that full range. Advancing copies retain the full
 * flush: an earlier iteration can have written code without its final word
 * landing on a tag, and the later full flush is what makes that sequence safe.
 * Thumb block stores always write back, so they follow the same fallback. */
/* WITHDRAWN: HARDENING OF THIS FINGERPRINT.
 *
 * The predicate below is narrow but not sound: an ARM STMIA lr,{r0,r1} whose
 * highest written word sits 0x3c bytes past the writer is a 32-bit opcode match
 * plus an offset, and nothing in it is specific to a sound engine.  I added
 * three requirements -- writer and target both in IWRAM, one latched writer pc,
 * and N repetitions before activating -- on the reasoning that all three are
 * structurally true of M4A's SoundMainRAM.
 *
 * REMOVED after a hardware session on 2026-09-18 showed degraded performance.
 * This function is the gate on selective invalidation, so any extra condition
 * that turns out not to hold for the ROM in hand costs the FULL FLUSH on every
 * mixer write -- which is a large, load-dependent slowdown and exactly what was
 * reported.  I had not verified on hardware that Unbound's writer satisfies the
 * IWRAM assumption, and a regression hunt is the wrong moment to be carrying an
 * unverified extra condition on the hot path.
 *
 * The risk it was addressing remains real but theoretical: a false match hands
 * a game selective retirement on a store whose earlier words bypassed the SMC
 * check.  Re-introducing it needs the writer pc and its region CONFIRMED from a
 * diagnostic build on hardware first -- ms0:/smchisto.txt reports storing pcs.
 *
 * Anything uncertain keeps the established full flush, which is the whole
 * point: this function's job is to say yes to one known routine, not to
 * classify stores in general. */
#ifdef SMC_WRITER_PROBE
/* DIAGNOSTIC ONLY, and in no build profile.  Record the distinct writer shapes
 * this function sees, so the fingerprint can be extended from measurement rather
 * than from a source comment.  Writes once, when the table first fills or at the
 * sample cap, then goes quiet -- this runs on the emulation thread and must not
 * turn into per-write file I/O. */
#define SWP_MAX 24
static u32 swp_pc[SWP_MAX], swp_addr[SWP_MAX], swp_op[SWP_MAX];
static u32 swp_hits[SWP_MAX], swp_used, swp_dumped, swp_seen;

static void swp_note(u32 pc, u32 addr, u32 op, int admitted)
{
  u32 i;
  FILE *f;
  swp_seen++;
  for (i = 0; i < swp_used; i++)
    if (swp_pc[i] == pc && swp_op[i] == op &&
        (swp_addr[i] - swp_pc[i]) == (addr - pc)) {
      swp_hits[i]++;
      goto maybe_dump;
    }
  if (swp_used < SWP_MAX) {
    i = swp_used++;
    swp_pc[i] = pc; swp_addr[i] = addr; swp_op[i] = op; swp_hits[i] = 1;
    /* `admitted` is folded into the opcode slot's sign bit-free companion via
     * hits; the dump prints the decode, which makes admission obvious. */
    (void)admitted;
  }
maybe_dump:
  if (swp_dumped || (swp_used < SWP_MAX && swp_seen < 20000))
    return;
  swp_dumped = 1;
  f = fopen("ms0:/PSP/GAME/GBADHOC-PERF/log/writer.txt", "w");
  if (!f)
    return;
  fprintf(f, "SMC writer shapes seen by smc_writer_safe_range\n");
  fprintf(f, "total traps=%u distinct=%u\n\n", (unsigned)swp_seen,
          (unsigned)swp_used);
  fprintf(f, "  writer_pc  target    delta  opcode    rlist base wb up pre  hits\n");
  for (i = 0; i < swp_used; i++) {
    u32 op = swp_op[i];
    fprintf(f,
      "  %08x   %08x  %5x  %08x  %04x  r%-3u %u  %u  %u   %u\n",
      (unsigned)swp_pc[i], (unsigned)swp_addr[i],
      (unsigned)(swp_addr[i] - swp_pc[i]), (unsigned)op,
      (unsigned)(op & 0xFFFF), (unsigned)((op >> 16) & 0xF),
      (unsigned)((op >> 21) & 1), (unsigned)((op >> 23) & 1),
      (unsigned)((op >> 24) & 1), (unsigned)swp_hits[i]);
  }
  fprintf(f, "\nUnbound admits: rlist=0003 base=r14 wb=0 up=1 pre=0 delta=3c\n");
  fclose(f);
}
#endif

/* How far past its own pc an M4A mixer patch store may reach and still be
 * recognised.  Measured maximum on Heart & Soul is 0x148; Unbound's is 0x3c. */
#define SMC_MIXER_PATCH_SPAN 0x200

static int smc_writer_safe_range(u32 *low, u32 *high)
{
  u32 pc = GBA_PC(reg[REG_PC]);
  u32 addr = smc_last_write_addr;
  u32 op;
  u8 *blk;

#ifdef SMC_PARTIAL_SAFE_CONTROL
  (void)low;
  (void)high;
  return 0;
#endif

  /* The first correctness oracle proved that selectively retiring arbitrary
   * single stores is still too broad for Heart & Soul. Only admit the exact
   * class needed by Unbound; all Thumb and non-STM writes keep full flushes. */
  if (reg[REG_CPSR] & 0x20)
    return 0;

  pc -= 4;
  blk = memory_map_read[pc >> 15];
  if (!blk)
    blk = load_gamepak_page((pc >> 15) & 0x3FF);
  op = readaddress32(blk, pc & 0x7FFF);
#ifdef SMC_WRITER_PROBE
  if ((op & 0x0E000000) == 0x08000000)
    swp_note(pc, addr, op, 0);
#endif
  if ((op & 0x0E000000) == 0x08000000) {
    u32 list, count = 0;
    /* Loads cannot raise this store trap; treat an inconsistent decode as a
     * reason to use the established full flush. */
    if ((op & 0x00100000) || (op & 0x00200000))
      return 0;
    /* Admit M4A's SoundMainRAM patch stores: a non-writeback ARM STMIA through
     * LR into the nearby generated mixer body.
     *
     * This used to require the checked word to sit EXACTLY 0x3c bytes past the
     * writer, which is CFRU's (Unbound's) layout, and to be exactly {r0,r1}.
     * Heart & Soul runs the same routine family elsewhere and therefore kept the
     * coarse path on every mixer write.  Measured with SMC_WRITER_PROBE on
     * heart_soul_heavy, 2815 traps: FOUR writer sites across SIXTEEN deltas
     * (0x48..0x148), two storing two words and two storing four, the dominant
     * one matching CFRU's shape on every field except the delta and accounting
     * for 66% of traps.
     *
     * The delta was never a correctness condition.  The range returned below is
     * derived from the decoded register count and covers every word the store
     * writes, which is precisely the hazard a wider match is feared to expose --
     * see the note above this function.  For STMIA with U=1 and P=0 the trapping
     * address is the highest word, which is what makes that subtraction right for
     * any count.  So the exact offset was conservatism: one known routine rather
     * than one known routine FAMILY.
     *
     * WRITEBACK (bit 21) IS NOW CHECKED, and that is not cosmetic.  The old
     * condition never tested it because the exact delta already excluded every
     * writeback store it could have met; with a range that accident is gone.  The
     * safety argument holds only for non-writeback stores, because an advancing
     * copy can write code whose final word never lands on a tag.  The probe found
     * exactly those -- base r0, list 07f8, W set -- copying into the stack, and
     * they must keep the full flush.
     *
     * The delta stays bounded: a self-patching mixer writes near itself, and a
     * store reaching far past its own pc is a different shape.  That bound is
     * identity conservatism, not correctness, but it limits what a false match
     * could reach. */
    if (((op >> 16) & 0xF) != REG_LR ||
        (op & 0x01000000) || !(op & 0x00800000) || (op & 0x00400000) ||
        (op & 0x00200000) ||                 /* W: writeback must be clear */
        addr <= pc || addr - pc > SMC_MIXER_PATCH_SPAN)
      return 0;
    list = op & 0xFFFF;
    while (list) {
      count += list & 1;
      list >>= 1;
    }
    if (!count)
      return 0;
    *low = addr - (count - 1) * 4;
    *high = addr + 4;
    return 1;
  }
  return 0;
}

#ifdef SMC_PARTIAL_SAFE_FRAMEFULL
static u32 smc_partial_last_full_frame = ~0u;
#endif

#ifdef SMC_GATES
/* The ordinary gate is harvested from smc_last_write_addr, which is the
 * highest word written by an ARM STM.  SoundMainRAM's checked store writes
 * two words, so placing the boundary there still lets a translated block
 * span the first modified instruction.  The exact writer decoder above gives
 * us the complete range; for that proven layout, anchor the gate at the first
 * word instead.
 *
 * Keep the established high-word gate as well.  It is part of the existing
 * block/timing layout; the deterministic audio oracle detects removing it
 * even when both reference video frames remain identical.  The caller still
 * performs a full RAM-cache flush immediately after adding this one-time
 * boundary, so no block translated without it survives. */
static int smc_partial_install_range_gate(u32 low, u32 high)
{
  u32 i, found_high = 0, found_low = 0;
  int changed = 0;

  low &= ~3u;
  high = (high - 4) & ~3u;

  for (i = 0; i < translation_gate_targets; i++) {
    found_high |= translation_gate_target_pc[i] == high;
    found_low  |= translation_gate_target_pc[i] == low;
  }

  /* Preserve the original checked-word boundary first, then add the range
   * start.  Installing both in one event lets the mandatory full flush below
   * establish a single, deterministic block layout. */
  if (!found_high && translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = high;
    SMC_GATE_MAP_ADD(high);
    changed = 1;
  }
  if (!found_low && translation_gate_targets < MAX_TRANSLATION_GATES) {
    translation_gate_target_pc[translation_gate_targets++] = low;
    SMC_GATE_MAP_ADD(low);
    changed = 1;
  }

  return changed;
}
#endif

#elif defined(SMC_PARTIAL)
/* Was the store that raised this SMC a BLOCK COPY -- a multi-word store
 * that advances its base register through memory?
 *
 * Partial invalidation assumes every write into translated code is
 * reported.  Block stores break that: every word but the last goes through
 * execute_aligned_store32, which has no SMC check, and only the final word
 * reaches execute_store_u32.  A copy loop therefore overwrites code in
 * chunks, and a chunk whose final word lands on data is never reported at
 * all.  Upstream's full flush hides this -- any later event in the same copy
 * wipes everything -- but retiring single blocks leaves the unreported
 * chunks running their old translations.
 *
 * Pokemon Heart & Soul copies a routine into IWRAM with STMIA r0!, {r3-r10}.
 * SMC_GATES splits the routine at the reported addresses, so the unreported
 * words sit in blocks that are never retired; the routine runs a mix of new
 * and old code, computes a garbage length and fills the whole address space
 * (a white screen for ~12 s, then corrupted memory).  Either flag alone
 * boots the game.
 *
 * A copy loop moves its base (ARM STM with writeback, Thumb STMIA/PUSH);
 * those get the full flush, exactly as before partial invalidation existed.
 * An in-place patch does not -- Unbound's sound driver rewrites its own code
 * with STMIA lr, {r0,r1} -- and keeps the fast path unchanged.
 *
 * reg[REG_PC] was set by the smc_write stub from the store emitter's reg_a2:
 * the writer's PC + 4 (ARM) or + 2 (Thumb), for single and block stores
 * alike.  The opcode is fetched the way the translator fetches it. */
/* Did the instruction that triggered this SMC check write a whole register
 * list?  If so its earlier words bypassed the check entirely (see below), so
 * the partial flush is not safe and the caller must wipe the RAM cache. */
static int smc_writer_is_block_copy(void)
{
  u32 pc = reg[REG_PC];
  u32 op;
  u8 *blk;

  if (reg[REG_CPSR] & 0x20)
  {
    pc -= 2;
    blk = memory_map_read[pc >> 15];
    if (!blk)
      blk = load_gamepak_page((pc >> 15) & 0x3FF);
    op = readaddress16(blk, pc & 0x7FFF);
    return (op & 0xF800) == 0xC000 ||    /* STMIA rb!, {rlist} */
           (op & 0xFE00) == 0xB400;      /* PUSH {rlist[, lr]} */
  }

  pc -= 4;
  blk = memory_map_read[pc >> 15];
  if (!blk)
    blk = load_gamepak_page((pc >> 15) & 0x3FF);
  op = readaddress32(blk, pc & 0x7FFF);
  /* ANY ARM block store, writeback or not.
   *
   * 2.0.2 tested for writeback (W=1, mask 0x0E300000 / 0x08200000) because
   * the Heart & Soul boot hang was a register-list copy loop that used it.
   * That was too narrow and shipped the same bug in a second form: EVERY
   * non-final word of an ARM STM is emitted as execute_aligned_store32
   * (mips_emit.h arm_block_memory_store), which carries no SMC check at all,
   * regardless of writeback.  Only the final word reaches execute_store_u32
   * and lands here.  So `STMIA r0, {r1-r8}` over code answered "not a block
   * copy", took the partial flush around the last word alone, and left the
   * earlier words' stale translations live — executing freed code, which on
   * hardware is an unhandled exception and an instant power-off.
   *
   * Bits 27-25 == 100 selects block data transfer; L (bit 20) == 0 selects
   * store.  P/U/S/W are all irrelevant to whether the copy skipped the check.
   * Over-detecting only costs a full flush instead of a partial one, which is
   * slower but never wrong; under-detecting corrupts the cache. */
  return (op & 0x0E100000) == 0x08000000; /* any ARM STM, any cond */
}
#endif

/* THE OPTIMISATION THAT LOOKS OBVIOUS HERE AND IS WRONG.
 *
 * Full-flushing on every block copy costs real speed: battle entry and saving
 * in Heart & Soul hammer IWRAM, so the flushes land in bursts and the frame
 * pacer visibly over-corrects afterwards (59 -> 65 fps, the player character
 * speeds up, then it settles).  The tempting fix is that only the words THIS
 * copy wrote can hold stale translations, and the opcode names exactly how
 * many there were -- so retire just [last - (n-1)*4, last] and leave the rest
 * of the cache alone.  The geometry is right: both drivers in mips_emit.h
 * walk the register list ascending and mark the HIGHEST set register as the
 * final store, the only one that reaches execute_store_u32, so the address
 * recorded in smc_last_write_addr is the top of the copy.
 *
 * Implementing it as a LOOP over flush_translation_cache_ram_block() does not
 * work, and was tried on hardware 2026-09-11: Heart & Soul went back to a
 * white screen on boot, the exact pre-2.0.2 symptom.  That routine ZEROES the
 * block's tag run as its last act, so the first call blinds every later one --
 * they read tag 0, conclude "no code here", return early, and leave live
 * blocks that still cover the range holding stale translations.  This is the
 * v1/v4 failure already in the SMC notes ("retiring punches holes in runs
 * other blocks depend on"), rediscovered by not reading them.
 *
 * A range retire has to be ONE pass: walk back once from the low end, retire
 * through the high end, clear tags once at the finish.  Until that exists,
 * flush everything -- the speed is worth nothing if the game does not boot. */

#ifdef SMC_WRITE_HISTO
/* Where do the SMC flushes actually COME from?  A battle measured 10122 of
 * them and six of eight gate slots sat near sp, which looked like stack
 * traffic -- but clamping the scan at sp changed nothing, so the guess was
 * wrong.  Bucket both the WRITE address and the PC of the storing
 * instruction (mips_stub.S smc_write stores it to REG_PC before calling in)
 * so the next change is aimed at measured hot spots instead of a hunch. */
/* Word-granular view of the hot region.  256B buckets cannot tell "copies a
 * routine" from "updates one data word that happens to sit inside a block
 * scan_block tagged as code" -- and those want opposite fixes. */
/* WHICH block scan covers the hot data word?  The SMC check fires on any
 * non-zero tag, including the interior CODE_TAG_BLOCK16 that scan_block
 * writes for every address it scans -- so 0x0300168c need not be a block
 * START to cause flushes, it only has to fall inside some block's scanned
 * range.  Record the distinct blocks whose [start,end) spans it. */
#define SMC_COVER_ADDR 0x3001404u
#define SMC_COVER_MAX 24
static u32 smc_cov_s[SMC_COVER_MAX], smc_cov_e[SMC_COVER_MAX];
static u32 smc_cov_r[SMC_COVER_MAX], smc_cov_n[SMC_COVER_MAX];
static u32 smc_cov_used;
/* Does the hot word actually CHANGE?  SMC_SKIP_SAME (skip the flush when the
 * store writes an identical value) white-screened H&S, but its premise was
 * never tested on this game.  If the value is the same every time, all these
 * flushes are unnecessary and fixing that filter recovers everything; if it
 * genuinely changes, the whole avenue is dead.  The store has already landed
 * by the time we get here, so compare against what we saw last flush. */
static u32 smc_val_prev, smc_val_seen, smc_val_same, smc_val_diff;
void smc_cover_note(u32 s, u32 e, u32 reason)
{
  u32 k;
  if (!(s <= SMC_COVER_ADDR && SMC_COVER_ADDR < e)) return;
  for (k = 0; k < smc_cov_used; k++)
    if (smc_cov_s[k] == s && smc_cov_e[k] == e) { smc_cov_n[k]++; return; }
  if (smc_cov_used >= SMC_COVER_MAX) return;
  smc_cov_s[smc_cov_used] = s; smc_cov_e[smc_cov_used] = e;
  smc_cov_r[smc_cov_used] = reason; smc_cov_n[smc_cov_used] = 1;
  smc_cov_used++;
}

#define SMC_FINE_BASE 0x3001400u
#define SMC_FINE_WORDS 256                 /* covers 0x3001400..0x30017ff */
static u32 smc_h_fine_w[SMC_FINE_WORDS];   /* write address */
static u32 smc_h_fine_p[SMC_FINE_WORDS];   /* storing pc */
static u32 smc_h_addr_iw[128], smc_h_addr_ew[1024];
static u32 smc_h_pc_iw[128], smc_h_pc_other;
static u32 smc_h_n;
static void smc_histo_note(u32 addr, u32 storepc)
{
  if (addr >= 0x3000000) smc_h_addr_iw[(addr & 0x7FFF) >> 8]++;
  else                   smc_h_addr_ew[(addr & 0x3FFFF) >> 8]++;
  if (storepc >= 0x3000000 && storepc < 0x3008000)
    smc_h_pc_iw[(storepc & 0x7FFF) >> 8]++;
  else smc_h_pc_other++;
  if (addr - SMC_FINE_BASE < SMC_FINE_WORDS * 4u)
    smc_h_fine_w[(addr - SMC_FINE_BASE) >> 2]++;
  if (storepc - SMC_FINE_BASE < SMC_FINE_WORDS * 4u)
    smc_h_fine_p[(storepc - SMC_FINE_BASE) >> 2]++;
  if (addr == SMC_COVER_ADDR) {
    u32 cur = *(u32 *)(iwram + 0x8000 + (SMC_COVER_ADDR & 0x7FFF));
    if (smc_val_seen && cur == smc_val_prev) smc_val_same++; else smc_val_diff++;
    smc_val_prev = cur; smc_val_seen = 1;
  }
  if (++smc_h_n % 4000) return;
  {
    FILE *f = fopen("ms0:/smchisto.txt", "w");
    unsigned k;
    if (!f) return;
    /* Every counter here is a u32, which is `unsigned long` on this ABI while
     * %u/%x name `unsigned int`.  Same width on a PSP, undefined anywhere else;
     * cast rather than switch to %lu, which would be wrong on a target where
     * u32 is `unsigned int`. */
    fprintf(f, "smc flushes: %u\n", (unsigned)smc_h_n);
    fprintf(f, "unmappable block exits contained: %u\n",
            (unsigned)badjump_contained);
    fprintf(f, "-- WRITE addr, IWRAM 256B buckets --\n");
    for (k = 0; k < 128; k++) if (smc_h_addr_iw[k])
      fprintf(f, "  %08x %u\n", 0x3000000 + (k << 8),
              (unsigned)smc_h_addr_iw[k]);
    fprintf(f, "-- WRITE addr, EWRAM 256B buckets --\n");
    for (k = 0; k < 1024; k++) if (smc_h_addr_ew[k])
      fprintf(f, "  %08x %u\n", 0x2000000 + (k << 8),
              (unsigned)smc_h_addr_ew[k]);
    fprintf(f, "-- STORING pc, IWRAM 256B buckets --\n");
    for (k = 0; k < 128; k++) if (smc_h_pc_iw[k])
      fprintf(f, "  %08x %u\n", 0x3000000 + (k << 8),
              (unsigned)smc_h_pc_iw[k]);
    fprintf(f, "  (storing pc outside IWRAM: %u)\n", (unsigned)smc_h_pc_other);
    fprintf(f, "-- hot word %08x: same=%u changed=%u --\n",
            (unsigned)SMC_COVER_ADDR, (unsigned)smc_val_same,
            (unsigned)smc_val_diff);
    fprintf(f, "-- blocks whose scan COVERS %08x --\n",
            (unsigned)SMC_COVER_ADDR);
    for (k = 0; k < smc_cov_used; k++)
      fprintf(f, "  %08x..%08x  reason=%u  seen=%u\n",
              (unsigned)smc_cov_s[k], (unsigned)smc_cov_e[k],
              (unsigned)smc_cov_r[k], (unsigned)smc_cov_n[k]);
    fprintf(f, "-- FINE 0x%08x.. : word  writes  storing-pc --\n",
            (unsigned)SMC_FINE_BASE);
    for (k = 0; k < SMC_FINE_WORDS; k++)
      if (smc_h_fine_w[k] || smc_h_fine_p[k])
        fprintf(f, "  %08x  w=%-8u pc=%u\n",
                (unsigned)(SMC_FINE_BASE + (k << 2)),
                (unsigned)smc_h_fine_w[k], (unsigned)smc_h_fine_p[k]);
    fclose(f);
  }
}
#define SMC_HISTO_NOTE(a, p) smc_histo_note((a), (p))
#else
#define SMC_HISTO_NOTE(a, p) do { } while (0)
#endif

void flush_translation_cache_ram_smc(void)
{
#ifdef SMC_PARTIAL_SAFE
  u32 partial_low = 0, partial_high = 0;
  int range_safe = smc_writer_safe_range(&partial_low, &partial_high);
  int range_gate_changed = 0;
  int activating = range_safe && !smc_partial_active;
#endif
  flush_ram_smc++;
  SMC_HISTO_NOTE(smc_last_write_addr, reg[REG_PC]);
#ifdef SMC_GATES
#ifdef SMC_PARTIAL_SAFE
  if (range_safe)
    range_gate_changed = smc_partial_install_range_gate(partial_low,
                                                        partial_high);
  else
#endif
    smc_add_gate(smc_last_write_addr);
#endif
#ifdef SMC_PARTIAL_SAFE
  {
    if (activating) {
      /* ROM translations survive a RAM flush and may contain direct links
       * into RAM. Rebuild them once under the active indirect-link policy
       * before any selective retirement can occur. */
      smc_partial_active = 1;
      flush_translation_cache_rom();
    }
    /* The event that installs or moves the range-start gate gets a full
     * flush.  Subsequent events can retire selectively only after every live
     * block has therefore been scanned with that exact boundary. */
    int partial_ready = smc_partial_active && range_safe && !activating &&
                        !range_gate_changed &&
                        smc_partial_target_is_gated(partial_low);
#ifdef SMC_PARTIAL_SAFE_FRAMEFULL
    if (partial_ready && smc_partial_last_full_frame != frame_counter) {
      smc_partial_last_full_frame = frame_counter;
      flush_translation_cache_ram();
    } else
#endif
    if (partial_ready)
      flush_translation_cache_ram_range(partial_low, partial_high);
    else
      flush_translation_cache_ram();
  }
#elif defined(SMC_PARTIAL)
  if (smc_writer_is_block_copy())
    flush_translation_cache_ram();
  else
    flush_translation_cache_ram_block(smc_last_write_addr);
#else
  flush_translation_cache_ram();
#endif
}

void flush_translation_cache_ram_dma(void)
{
  flush_ram_dma++;
  flush_translation_cache_ram();
}

void flush_translation_cache_rom(void)
{
  /* We flush the generated code except for everything below the watermark. */
  flush_rom_total++;
  last_rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  rom_translation_ptr      = &rom_translation_cache[rom_cache_watermark];

  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
}

void init_dynarec_caches(void)
{
  /* Initialize caches so that we can start initalizing the emitter. */
  rom_translation_ptr = last_rom_translation_ptr = &rom_translation_cache[0];
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));

  ram_translation_ptr = last_ram_translation_ptr = &ram_translation_cache[0];
#ifdef SMC_PARTIAL_SAFE
  smc_partial_active = 0;
#endif
  memset(iwram, 0, 0x8000);
  memset(&ewram[0x40000], 0, 0x40000);

  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
  /* NO smc_gates_reset() HERE.  See flush_dynarec_caches below: a reset keeps
   * the same ROM, and ROM load already resets the table. */
}

/* WITHDRAWN: CLEARING THE GATE CANDIDATE TABLE HERE, AND IN
 * init_dynarec_caches.  This cost 2-16% of frame time on hardware.
 *
 * The proven defect was CROSS-ROM: a candidate saturated by one game promotes on
 * a single write from the next, because the stale remembered value counts as a
 * change.  That happens at ROM load, and gba_memory.c already calls
 * smc_gates_reset() there.  Extending it to savestate load, emulator reset,
 * cheat install and config change was my inference, and it is the expensive
 * kind of wrong.
 *
 * WHY IT COSTS SO MUCH.  Earning a gate needs SMC_GATE_MIN_SAMPLE observations
 * at ONE address, and smc_gate_earned runs once per SMC flush event -- not once
 * per store.  Measured on heart_soul_light, the hot mixer address 0300168c
 * produces about 12 events per 600 frames, so re-earning its gate from scratch
 * takes roughly 3200 frames.  Boot supplies those samples cheaply; a savestate
 * load at frame 30 threw them away and left the rest of the run in the no-gate
 * regime, which is the documented 457 KB/s re-translation behaviour.
 *
 * It shows up as MORE SMC EVENTS, not merely more CPU: hardware measured the
 * window count 33 -> 51, xlat 705 -> 831, and mean core time +21%.  The penalty
 * scales inversely with the mixer's write rate, which is why it was +16% on
 * heart_soul_light, +8% on unbound_double_high and +2% on unbound_rival_medium
 * -- the busier the mixer, the sooner the gate comes back.
 *
 * A savestate load does not change ROM, so the candidates remain evidence about
 * the right game.  What a state load DOES invalidate is smc_cand_val, the
 * remembered word at each address -- a real but much smaller hazard, and one
 * that cannot be closed by throwing away the hit counts that cost thousands of
 * frames to collect.  If it is ever worth closing, refresh the VALUES and keep
 * the counts. */
void flush_dynarec_caches(void)
{
  /* Flush ROM and RAM caches. */
  flush_translation_cache_rom();
  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
  flush_translation_cache_ram();
}
