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

extern "C" {
  #include "common.h"
  #include "cpu_instrument.h"
  #include "esp_heap_caps.h"
}

const u8 bit_count[256] =
{
  0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3,
  4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4,
  4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2,
  3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5,
  4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4,
  5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3,
  3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2,
  3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6,
  4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5,
  6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5,
  5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6,
  7, 7, 8
};


#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rd;                                                                   \
  (void)rn;                                                                   \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 imm;                                                                    \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm_ror = ((opcode >> 8) & 0xF) << 1;                                   \
  (void)rd;                                                                   \
  (void)rn;                                                                   \
  ror(imm, opcode & 0xFF, imm_ror);                                           \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src)                                             \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rd;                                                                   \
  (void)rm;                                                                   \
  (void)psr_pfield;                                                           \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_psr_imm(opcode)                                            \
  u32 imm;                                                                    \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  (void)rd;                                                                   \
  ror(imm, opcode & 0xFF, ((opcode >> 8) & 0x0F) * 2);                        \
  using_register(arm, rd, op_dest)                                            \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F;                                                     \
  using_register(arm, rn, branch_target)                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F;                                                     \
  (void)rn;                                                                   \
  using_register(arm, rd, op_dest);                                           \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rn = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rdhi, op_dest);                                         \
  using_register(arm, rdlo, op_dest);                                         \
  using_register(arm, rn, op_src);                                            \
  using_register(arm, rm, op_src)                                             \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_target)                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_offset)                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F);                      \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base)                                        \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF;                                               \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base)                                        \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F;                                                     \
  using_register(arm, rd, memory_target);                                     \
  using_register(arm, rn, memory_base);                                       \
  using_register(arm, rm, memory_offset)                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF;                                             \
  using_register(arm, rn, memory_base);                                       \
  using_register_list(arm, reg_list, 16)                                      \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)((u32)(opcode << 8))) >> 6                               \


#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_dest);                                         \
  using_register(thumb, rs, op_shift)                                         \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_dest);                                         \
  using_register(thumb, rn, op_src);                                          \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF;                                                    \
  using_register(thumb, ((opcode >> 8) & 0x07), op_dest)                      \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07);                          \
  (void)rd;                                                                   \
  using_register(thumb, rd, op_src_dest);                                     \
  using_register(thumb, rs, op_src)                                           \


#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, memory_target);                                   \
  using_register(thumb, rb, memory_base);                                     \
  using_register(thumb, ro, memory_offset)                                    \


#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07;                                                     \
  using_register(thumb, rd, memory_target);                                   \
  using_register(thumb, rb, memory_base)                                      \


#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F;                                                    \
  using_register(thumb, REG_SP, op_dest)                                      \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF;                                               \
  using_register_list(thumb, rlist, 8)                                        \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \


#define get_shift_register(dest)                                              \
  u32 shift = reg[(opcode >> 8) & 0x0F] & 0xFF;                               \
  using_register(arm, ((opcode >> 8) & 0x0F), op_shift);                      \
  dest = reg[rm];                                                             \
  if(rm == 15)                                                                \
    dest += 4                                                                 \


#define calculate_z_flag(dest)                                                \
  z_flag = (dest == 0)                                                        \

#define calculate_n_flag(dest)                                                \
  n_flag = ((signed)dest < 0)                                                 \

#define calculate_c_flag_sub(dest, src_a, src_b, carry)                       \
  c_flag = (carry) ? ((unsigned)src_b <= (unsigned)src_a) :                   \
                     ((unsigned)src_b < (unsigned)src_a);                     \

#define calculate_v_flag_sub(dest, src_a, src_b)                              \
  v_flag = (((src_a ^ src_b) & (~src_b ^ dest)) >> 31)

#define calculate_v_flag_add(dest, src_a, src_b)                              \
  v_flag = ((~((src_a) ^ (src_b)) & ((src_a) ^ (dest))) >> 31)

#define calculate_reg_sh()                                                    \
  u32 reg_sh = 0;                                                             \
  switch((opcode >> 4) & 0x07)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      reg_sh = reg[rm] << ((opcode >> 7) & 0x1F);                             \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSL reg */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = reg_sh << shift;                                             \
      else                                                                    \
        reg_sh = 0;                                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_sh = 0;                                                           \
      else                                                                    \
        reg_sh = reg[rm] >> imm;                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR reg */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = reg_sh >> shift;                                             \
      else                                                                    \
        reg_sh = 0;                                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x4:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
                                                                              \
      if(imm == 0)                                                            \
        reg_sh = (s32)reg_sh >> 31;                                           \
      else                                                                    \
        reg_sh = (s32)reg_sh >> imm;                                          \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR reg */                                                             \
    case 0x5:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift <= 31)                                                         \
        reg_sh = (s32)reg_sh >> shift;                                        \
      else                                                                    \
        reg_sh = (s32)reg_sh >> 31;                                           \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x6:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
                                                                              \
      if(imm == 0)                                                            \
        reg_sh = (reg[rm] >> 1) | (c_flag << 31);                             \
      else                                                                    \
        ror(reg_sh, reg[rm], imm);                                            \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR reg */                                                             \
    case 0x7:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      ror(reg_sh, reg_sh, shift);                                             \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_reg_sh_flags()                                              \
  u32 reg_sh = 0;                                                             \
  switch((opcode >> 4) & 0x07)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
                                                                              \
      if(imm != 0)                                                            \
      {                                                                       \
        c_flag = (reg_sh >> (32 - imm)) & 0x01;                               \
        reg_sh <<= imm;                                                       \
      }                                                                       \
                                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSL reg */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          if(shift == 32)                                                     \
            c_flag = reg_sh & 0x01;                                           \
          else                                                                \
            c_flag = 0;                                                       \
          reg_sh = 0;                                                         \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (32 - shift)) & 0x01;                           \
          reg_sh <<= shift;                                                   \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        c_flag = reg_sh >> 31;                                                \
        reg_sh = 0;                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        reg_sh >>= imm;                                                       \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR reg */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          if(shift == 32)                                                     \
            c_flag = (reg_sh >> 31) & 0x01;                                   \
          else                                                                \
            c_flag = 0;                                                       \
          reg_sh = 0;                                                         \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (shift - 1)) & 0x01;                            \
          reg_sh >>= shift;                                                   \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x4:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        reg_sh = (s32)reg_sh >> 31;                                           \
        c_flag = reg_sh & 0x01;                                               \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        reg_sh = (s32)reg_sh >> imm;                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR reg */                                                             \
    case 0x5:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        if(shift > 31)                                                        \
        {                                                                     \
          reg_sh = (s32)reg_sh >> 31;                                         \
          c_flag = reg_sh & 0x01;                                             \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          c_flag = (reg_sh >> (shift - 1)) & 0x01;                            \
          reg_sh = (s32)reg_sh >> shift;                                      \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x6:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      reg_sh = reg[rm];                                                       \
      if(imm == 0)                                                            \
      {                                                                       \
        u32 old_c_flag = c_flag;                                              \
        c_flag = reg_sh & 0x01;                                               \
        reg_sh = (reg_sh >> 1) | (old_c_flag << 31);                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        c_flag = (reg_sh >> (imm - 1)) & 0x01;                                \
        ror(reg_sh, reg_sh, imm);                                             \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR reg */                                                             \
    case 0x7:                                                                 \
    {                                                                         \
      get_shift_register(reg_sh);                                             \
      if(shift != 0)                                                          \
      {                                                                       \
        c_flag = (reg_sh >> (shift - 1)) & 0x01;                              \
        ror(reg_sh, reg_sh, shift);                                           \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_reg_offset()                                                \
  u32 reg_offset = 0;                                                         \
  switch((opcode >> 5) & 0x03)                                                \
  {                                                                           \
    /* LSL imm */                                                             \
    case 0x0:                                                                 \
    {                                                                         \
      reg_offset = reg[rm] << ((opcode >> 7) & 0x1F);                         \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* LSR imm */                                                             \
    case 0x1:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = 0;                                                       \
      else                                                                    \
        reg_offset = reg[rm] >> imm;                                          \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ASR imm */                                                             \
    case 0x2:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = (s32)reg[rm] >> 31;                                      \
      else                                                                    \
        reg_offset = (s32)reg[rm] >> imm;                                     \
      break;                                                                  \
    }                                                                         \
                                                                              \
    /* ROR imm */                                                             \
    case 0x3:                                                                 \
    {                                                                         \
      u32 imm = (opcode >> 7) & 0x1F;                                         \
      if(imm == 0)                                                            \
        reg_offset = (reg[rm] >> 1) | (c_flag << 31);                         \
      else                                                                    \
        ror(reg_offset, reg[rm], imm);                                        \
      break;                                                                  \
    }                                                                         \
  }                                                                           \

#define calculate_flags_add(dest, src_a, src_b)                               \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest);                                                     \
  calculate_v_flag_add(dest, src_a, src_b)                                    \

#define calculate_flags_sub(dest, src_a, src_b, carry)                        \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest);                                                     \
  calculate_c_flag_sub(dest, src_a, src_b, carry);                            \
  calculate_v_flag_sub(dest, src_a, src_b)                                    \

#define calculate_flags_logic(dest)                                           \
  calculate_z_flag(dest);                                                     \
  calculate_n_flag(dest)                                                      \

#define extract_flags()                                                       \
  n_flag = reg[REG_CPSR] >> 31;                                               \
  z_flag = (reg[REG_CPSR] >> 30) & 0x01;                                      \
  c_flag = (reg[REG_CPSR] >> 29) & 0x01;                                      \
  v_flag = (reg[REG_CPSR] >> 28) & 0x01;                                      \

#define collapse_flags()                                                      \
  reg[REG_CPSR] = (n_flag << 31) | (z_flag << 30) | (c_flag << 29) |          \
   (v_flag << 28) | (reg[REG_CPSR] & 0xFF)                                    \

#define check_pc_region()                                                     \
  new_pc_region = (reg[REG_PC] >> 15);                                        \
  if(new_pc_region != pc_region)                                              \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    pc_address_block = memory_map_read[new_pc_region];                        \
    touch_gamepak_page(pc_region);                                            \
                                                                              \
    if(!pc_address_block)                                                     \
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);                \
  }                                                                           \


#define arm_pc_offset(val)                                                    \
  reg[REG_PC] += val                                                          \

#define arm_next_instruction()                                                \
{                                                                             \
  arm_pc_offset(4);                                                           \
  goto skip_instruction;                                                      \
}                                                                             \

#define thumb_pc_offset(val)                                                  \
  reg[REG_PC] += val                                                          \


// It should be okay to still generate result flags, spsr will overwrite them.
// This is pretty infrequent (returning from interrupt handlers, et al) so
// probably not worth optimizing for.

#define check_for_interrupts()                                                \
  if((read_ioreg(REG_IE) & read_ioreg(REG_IF)) &&                             \
   read_ioreg(REG_IME) && ((reg[REG_CPSR] & 0x80) == 0))                      \
  {                                                                           \
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;                                  \
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];                                       \
    reg[REG_CPSR] = 0xD2;                                                     \
    reg[REG_PC] = 0x00000018;                                                 \
    set_cpu_mode(MODE_IRQ);                                                   \
    goto arm_loop;                                                            \
  }                                                                           \

#define arm_spsr_restore()                                                    \
  {                                                                           \
    if(reg[CPU_MODE] != MODE_USER && reg[CPU_MODE] != MODE_SYSTEM)            \
    {                                                                         \
      reg[REG_CPSR] = REG_SPSR(reg[CPU_MODE]);                                \
      extract_flags();                                                        \
      set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);                           \
      check_for_interrupts();                                                 \
    }                                                                         \
                                                                              \
    if(reg[REG_CPSR] & 0x20)                                                  \
      goto thumb_loop;                                                        \
  }                                                                           \

#define arm_spsr_restore_check()                                              \
  if(rd == REG_PC)                                                            \
  {                                                                           \
    arm_spsr_restore()                                                        \
  }                                                                           \

#define arm_spsr_restore_ldm_check()                                          \
  if (opcode & 0x8000)   /* PC is in the LDM reg list */                      \
  {                                                                           \
    arm_spsr_restore()                                                        \
  }                                                                           \

#define arm_data_proc_flags_reg()                                             \
  arm_decode_data_proc_reg(opcode);                                           \
  calculate_reg_sh_flags()                                                    \

#define arm_data_proc_reg()                                                   \
  arm_decode_data_proc_reg(opcode);                                           \
  calculate_reg_sh()                                                          \

#define arm_data_proc_flags_imm()                                             \
  arm_decode_data_proc_imm(opcode)                                            \
  if(imm_ror)                                                                 \
    c_flag = (imm >> 31);  /* imm is rotated already! */                      \

#define arm_data_proc_imm()                                                   \
  arm_decode_data_proc_imm(opcode)                                            \

#define arm_data_proc(expr, type)                                             \
{                                                                             \
  u32 dest;                                                                   \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  dest = expr;                                                                \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
}                                                                             \

#define flags_vars(src_a, src_b)                                              \
  u32 dest;                                                                   \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b                                                       \

#define arm_data_proc_logic_flags(expr, type)                                 \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_flags_##type();                                               \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}                                                                             \

#define arm_data_proc_add_flags(src_a, src_b, src_c, type)                    \
{                                                                             \
  u32 _sc = src_c;                                                            \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + _sb;                                                           \
  c_flag = (dest < _sb);                                                      \
  dest += _sc;                                                                \
  c_flag |= (dest < _sc);                                                     \
  calculate_flags_add(dest, _sa, _sb);                                        \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}

#define arm_data_proc_sub_flags(src_a, src_b, src_c, type)                    \
{                                                                             \
  u32 _sc = src_c;                                                            \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + (~(_sb)) + _sc;                                                \
  calculate_flags_sub(dest, _sa, _sb, _sc);                                   \
  arm_pc_offset(-4);                                                          \
  reg[rd] = dest;                                                             \
  arm_spsr_restore_check();                                                   \
}                                                                             \

#define arm_data_proc_test_logic(expr, type)                                  \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_flags_##type();                                               \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_data_proc_test_add(src_a, src_b, type)                            \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa + _sb;                                                           \
  c_flag = (dest < _sb);                                                      \
  calculate_flags_add(dest, _sa, _sb);                                        \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_data_proc_test_sub(src_a, src_b, type)                            \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_proc_##type();                                                     \
  flags_vars(src_a, src_b);                                                   \
  dest = _sa - _sb;                                                           \
  calculate_flags_sub(dest, _sa, _sb, 1);                                     \
  arm_pc_offset(-4);                                                          \
}                                                                             \

#define arm_multiply_flags_yes(_dest)                                         \
  calculate_z_flag(_dest);                                                    \
  calculate_n_flag(_dest);                                                    \

#define arm_multiply_flags_no(_dest)                                          \

#define arm_multiply_long_flags_yes(_dest_lo, _dest_hi)                       \
  z_flag = (_dest_lo == 0) & (_dest_hi == 0);                                 \
  calculate_n_flag(_dest_hi)                                                  \

#define arm_multiply_long_flags_no(_dest_lo, _dest_hi)                        \

#define arm_multiply(add_op, flags)                                           \
{                                                                             \
  u32 dest;                                                                   \
  arm_decode_multiply();                                                      \
  dest = (reg[rm] * reg[rs]) add_op;                                          \
  arm_multiply_flags_##flags(dest);                                           \
  reg[rd] = dest;                                                             \
  arm_pc_offset(4);                                                           \
}                                                                             \

#define arm_multiply_long_addop(type)                                         \
  + ((type##64)((((type##64)reg[rdhi]) << 32) | reg[rdlo]));                  \

#define arm_multiply_long(add_op, flags, type)                                \
{                                                                             \
  type##64 dest;                                                              \
  u32 dest_lo;                                                                \
  u32 dest_hi;                                                                \
  arm_decode_multiply_long();                                                 \
  dest = ((type##64)((type##32)reg[rm]) *                                     \
   (type##64)((type##32)reg[rn])) add_op;                                     \
  dest_lo = (u32)dest;                                                        \
  dest_hi = (u32)(dest >> 32);                                                \
  arm_multiply_long_flags_##flags(dest_lo, dest_hi);                          \
  reg[rdlo] = dest_lo;                                                        \
  reg[rdhi] = dest_hi;                                                        \
  arm_pc_offset(4);                                                           \
}                                                                             \

// Index by PRS fields (1 and 4 only!) and User-Privileged mode
// In user mode some bits are read only
// Bit #4 is always set to one (so all modes are 1XXXX)
// Reserved bits are always zero and cannot be modified
const u32 cpsr_masks[4][2] =
{
  // User, Privileged
  {0x00000000, 0x00000000},
  {0x00000020, 0x000000EF},
  {0xF0000000, 0xF0000000},
  {0xF0000020, 0xF00000EF}
};

// SPSR is always a privileged instruction
const u32 spsr_masks[4] = { 0x00000000, 0x000000EF, 0xF0000000, 0xF00000EF };

#define arm_psr_read(dummy, psr_reg)                                          \
  collapse_flags();                                                           \
  reg[rd] = psr_reg                                                           \

#define arm_psr_store_cpsr(source)                                            \
  const u32 store_mask = cpsr_masks[psr_pfield][PRIVMODE(reg[CPU_MODE])];     \
  reg[REG_CPSR] = (source & store_mask) | (reg[REG_CPSR] & (~store_mask));    \
  extract_flags();                                                            \
  if(store_mask & 0xFF)                                                       \
  {                                                                           \
    set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);                             \
    check_for_interrupts();                                                   \
  }                                                                           \

#define arm_psr_store_spsr(source)                                            \
  const u32 store_mask = spsr_masks[psr_pfield];                              \
  u32 _psr = REG_SPSR(reg[CPU_MODE]);                                         \
  REG_SPSR(reg[CPU_MODE]) = (source & store_mask) | (_psr & (~store_mask))    \

#define arm_psr_store(source, psr_reg)                                        \
  arm_psr_store_##psr_reg(source)                                             \

#define arm_psr_src_reg reg[rm]

#define arm_psr_src_imm imm

#define arm_psr(op_type, transfer_type, psr_reg)                              \
{                                                                             \
  arm_decode_psr_##op_type(opcode);                                           \
  arm_pc_offset(4);                                                           \
  arm_psr_##transfer_type(arm_psr_src_##op_type, psr_reg);                    \
}                                                                             \

#define arm_data_trans_reg()                                                  \
  arm_decode_data_trans_reg();                                                \
  calculate_reg_offset()                                                      \

#define arm_data_trans_imm()                                                  \
  arm_decode_data_trans_imm()                                                 \

#define arm_data_trans_half_reg()                                             \
  arm_decode_half_trans_r()                                                   \

#define arm_data_trans_half_imm()                                             \
  arm_decode_half_trans_of()                                                  \

#define aligned_address_mask8  0xF0000000
#define aligned_address_mask16 0xF0000001
#define aligned_address_mask32 0xF0000003

#define fast_read_memory(size, type, addr, dest, readfn)                      \
{                                                                             \
  u8 *map;                                                                    \
  u32 _address = addr;                                                        \
                                                                              \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_nseq[region][(size - 8) / 16];                 \
    STATS_MEMORY_ACCESS(read, type, region);                                  \
  }                                                                           \
                                                                              \
  if (                                                                        \
     (((_address >> 24) == 0) && (reg[REG_PC] >= 0x4000)) ||  /* BIOS read */ \
     (_address & aligned_address_mask##size) ||      /* Unaligned access */   \
     !(map = memory_map_read[_address >> 15])        /* Unmapped memory */    \
  )                                                                           \
  {                                                                           \
    dest = (type)(readfn)(_address);                                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    /* Aligned and mapped read */                                             \
    dest = (type)readaddress##size(map, (_address & 0x7FFF));                 \
  }                                                                           \
}                                                                             \

#define fast_write_memory(size, type, address, value)                         \
{                                                                             \
  u32 _address = (address) & ~(aligned_address_mask##size & 0x03);            \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_nseq[region][(size - 8) / 16];                 \
    STATS_MEMORY_ACCESS(write, type, region);                                 \
  }                                                                           \
                                                                              \
  cpu_alert |= write_memory##size(_address, value);                           \
}                                                                             \

#define load_aligned32(address, dest)                                         \
{                                                                             \
  u32 _address = address;                                                     \
  u8 *map = memory_map_read[_address >> 15];                                  \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_seq[region][1];                                \
    STATS_MEMORY_ACCESS(read, u32, region);                                   \
  }                                                                           \
  if(_address < 0x10000000 && map)                                            \
  {                                                                           \
    dest = readaddress32(map, _address & 0x7FFF);                             \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = read_memory32(_address);                                           \
  }                                                                           \
}                                                                             \

#define store_aligned32(address, value)                                       \
{                                                                             \
  u32 _address = address;                                                     \
  if(_address < 0x10000000)                                                   \
  {                                                                           \
    /* Account for cycles and other stats */                                  \
    u8 region = _address >> 24;                                               \
    cycles_remaining -= ws_cyc_seq[region][1];                                \
    STATS_MEMORY_ACCESS(write, u32, region);                                  \
  }                                                                           \
  cpu_alert |= write_memory32(_address, value);                               \
}                                                                             \

#define load_memory_u8(address, dest)                                         \
  fast_read_memory(8, u8, address, dest, read_memory8)                        \

/* LDRH zero-extends an aligned halfword, but a misaligned load rotates the
 * 32-bit result (ROR #8, see read_memory16). With type u16 the cast truncated
 * that rotation -> jsmolka thumb-211 failed and IWRAM game code (Pokémon) could
 * hang. The JIT already uses u32 (jit_ld16). Same here: u32 = full value. */
#define load_memory_u16(address, dest)                                        \
  fast_read_memory(16, u32, address, dest, read_memory16)                     \

#define load_memory_u32(address, dest)                                        \
  fast_read_memory(32, u32, address, dest, read_memory32)                     \

#define load_memory_s8(address, dest)                                         \
  fast_read_memory(8, s8, address, dest, read_memory8)                        \

#define load_memory_s16(address, dest)                                        \
  fast_read_memory(16, s16, address, dest, read_memory16_signed)              \

#define store_memory_u8(address, value)                                       \
  fast_write_memory(8, u8, address, value)                                    \

#define store_memory_u16(address, value)                                      \
  fast_write_memory(16, u16, address, value)                                  \

#define store_memory_u32(address, value)                                      \
  fast_write_memory(32, u32, address, value)                                  \

#define no_op                                                                 \

#define arm_access_memory_writeback_yes(off_op)                               \
  reg[rn] = address off_op                                                    \

#define arm_access_memory_writeback_no(off_op)                                \

#define arm_access_memory_pc_preadjust_load()                                 \

#define arm_access_memory_pc_preadjust_store()                                \
  u32 reg_op = reg[rd];                                                       \
  if(rd == 15)                                                                \
    reg_op += 4                                                               \

#define load_reg_op reg[rd]                                                   \

#define store_reg_op reg_op                                                   \

#define arm_access_memory(access_type, off_op, off_type, mem_type,            \
 wb, wb_off_op)                                                               \
{                                                                             \
  arm_pc_offset(8);                                                           \
  arm_data_trans_##off_type();                                                \
  u32 address = reg[rn] off_op;                                               \
  arm_access_memory_pc_preadjust_##access_type();                             \
                                                                              \
  arm_pc_offset(-4);                                                          \
  arm_access_memory_writeback_##wb(wb_off_op);                                \
  access_type##_memory_##mem_type(address, access_type##_reg_op);             \
}                                                                             \

// Excutes an LDM/STM instruction

typedef enum { AccLoad, AccStore } AccMode;
typedef enum { AddrPreInc, AddrPreDec, AddrPostInc, AddrPostDec } AddrMode;

template<AccMode mode, bool writeback, bool sbit, AddrMode addr_mode>
inline cpu_alert_type exec_arm_block_mem(u32 rn, u32 reglist, s32 &cycles_remaining) {
  cpu_alert_type cpu_alert = CPU_ALERT_NONE;
  // Register register usage stats.
  using_register(arm, rn, memory_base);
  using_register_list(arm, reglist, 16);

  // Calcualte base address.
  u32 base = reg[rn];
  u32 numops = (bit_count[reglist >> 8] + bit_count[reglist & 0xFF]);
  s32 addr_off = (addr_mode == AddrPreInc || addr_mode == AddrPostInc) ? 4 : -4;  // Address incr/decr amount.
  u32 endaddr = base + addr_off * numops;

  u32 address = (addr_mode == AddrPreInc)  ? base + 4 :
                (addr_mode == AddrPostInc) ? base :
                (addr_mode == AddrPreDec)  ? endaddr : endaddr + 4;
  address &= ~3U;

  // If sbit is set, change to user mode and back, so to write the user regs.
  // However for LDM {PC} we restore CPSR from SPSR.
  // TODO: implement CPSR restore, only USER mode is now implemented.
  u32 old_cpsr = reg[REG_CPSR];
  if (sbit && (mode == AccStore || rn != REG_PC))
    set_cpu_mode(MODE_USER);

  // If base is in the reglist and writeback is enabled, the value of the
  // written register depends on the write cycle (ARM7TDM manual 4.11.6).
  // If the register is the first, the written value is the original value,
  // otherwise the update base register is written. For LDM loaded date
  // takes always precendence.
  bool wrbck_base = (1 << rn) & reglist;
  bool base_first = (((1 << rn) - 1) & reglist) == 0;
  bool writeback_first = (mode == AccLoad) || !(wrbck_base && base_first);

  if (writeback && writeback_first)
    reg[rn] = endaddr;

  arm_pc_offset(4);  // Advance PC

  for (u32 i = 0; i < 16; i++)  {
    if ((reglist >> i) & 0x01) {
      if (mode == AccLoad) {
        load_aligned32(address, reg[i]);
      } else {
        store_aligned32(address, (i == REG_PC) ? reg[REG_PC] + 4 : reg[i]);
      }
      address += 4;
    }
  }

  if (writeback && !writeback_first)
    reg[rn] = endaddr;

  if (sbit && (mode == AccStore || rn != REG_PC))
    set_cpu_mode(cpu_modes[old_cpsr & 0xF]);

  return cpu_alert;
}

template<AccMode mode, AddrMode addr_mode>
inline cpu_alert_type exec_thumb_block_mem(u32 rn, u32 reglist, s32 &cycles_remaining) {
  cpu_alert_type cpu_alert = CPU_ALERT_NONE;
  // Register register usage stats.
  using_register(arm, rn, memory_base);
  using_register_list(arm, reglist, 16);

  // Calcualte base address.
  u32 base = reg[rn];
  u32 numops = bit_count[reglist & 0xFF] + (bit_count[reglist >> 8] ? 1 : 0);
  s32 addr_off = (addr_mode == AddrPreInc || addr_mode == AddrPostInc) ? 4 : -4;  // Address incr/decr amount.
  u32 endaddr = base + addr_off * numops;

  u32 address = (addr_mode == AddrPreInc)  ? base + 4 :
                (addr_mode == AddrPostInc) ? base :
                (addr_mode == AddrPreDec)  ? endaddr : endaddr + 4;
  address &= ~3U;

  // Similar to the ARM version, just a bit simpler. See above.
  bool wrbck_base = (1 << rn) & reglist;
  bool base_first = (((1 << rn) - 1) & reglist) == 0;
  bool writeback_first = (mode == AccLoad) || !(wrbck_base && base_first);

  if (writeback_first)
    reg[rn] = endaddr;

  thumb_pc_offset(2);  // Advance PC

  if (mode == AccLoad) {
    for (u32 i = 0; i < 8; i++)  {
      if ((reglist >> i) & 0x01) {
        load_aligned32(address, reg[i]);
        address += 4;
      }
    }
    if (reglist & (1 << REG_PC)) {
      load_aligned32(address, reg[REG_PC]);
      reg[REG_PC] &= ~0x01;
    }
  } else {
    for (u32 i = 0; i < 8; i++)  {
      if ((reglist >> i) & 0x01) {
        store_aligned32(address, reg[i]);
        address += 4;
      }
    }
    if (reglist & (1 << REG_LR)) {
      store_aligned32(address, reg[REG_LR]);
    }
  }

  if (!writeback_first)
    reg[rn] = endaddr;

  return cpu_alert;
}

#define arm_swap(type)                                                        \
{                                                                             \
  arm_decode_swap();                                                          \
  u32 temp;                                                                   \
  load_memory_##type(reg[rn], temp);                                          \
  store_memory_##type(reg[rn], reg[rm]);                                      \
  reg[rd] = temp;                                                             \
  arm_pc_offset(4);                                                           \
}                                                                             \

// Types: add_sub, add_sub_imm, alu_op, imm
// Affects N/Z/C/V flags

#define thumb_add(type, dest_reg, src_a, src_b, src_c)                        \
{                                                                             \
  const u32 _sc = src_c;                                                      \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa + _sb;                                                       \
  c_flag = (dest < _sb);                                                      \
  dest += _sc;                                                                \
  c_flag |= (dest < _sc);                                                     \
  calculate_flags_add(dest, _sa, _sb);                                        \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_add_noflags(type, dest_reg, src_a, src_b)                       \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = (src_a) + (src_b);                                               \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_sub(type, dest_reg, src_a, src_b, src_c)                        \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  const u32 _sc = src_c;                                                      \
  u32 dest = _sa + (~_sb) + _sc;                                              \
  calculate_flags_sub(dest, _sa, _sb, _sc);                                   \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

// Affects N/Z flags

#define thumb_logic(type, dest_reg, expr)                                     \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  reg[dest_reg] = dest;                                                       \
  thumb_pc_offset(2);                                                         \
}                                                                             \

// Decode types: shift, alu_op
// Operation types: lsl, lsr, asr, ror
// Affects N/Z/C flags

#define thumb_shift_lsl_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      if(shift == 32)                                                         \
        c_flag = dest & 0x01;                                                 \
      else                                                                    \
        c_flag = 0;                                                           \
      dest = 0;                                                               \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (32 - shift)) & 0x01;                                 \
      dest <<= shift;                                                         \
    }                                                                         \
  }                                                                           \

#define thumb_shift_lsr_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      if(shift == 32)                                                         \
        c_flag = dest >> 31;                                                  \
      else                                                                    \
        c_flag = 0;                                                           \
      dest = 0;                                                               \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (shift - 1)) & 0x01;                                  \
      dest >>= shift;                                                         \
    }                                                                         \
  }                                                                           \

#define thumb_shift_asr_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    if(shift > 31)                                                            \
    {                                                                         \
      dest = (s32)dest >> 31;                                                 \
      c_flag = dest & 0x01;                                                   \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      c_flag = (dest >> (shift - 1)) & 0x01;                                  \
      dest = (s32)dest >> shift;                                              \
    }                                                                         \
  }                                                                           \

#define thumb_shift_ror_reg()                                                 \
  u32 shift = reg[rs];                                                        \
  u32 dest = reg[rd];                                                         \
  if(shift != 0)                                                              \
  {                                                                           \
    c_flag = (dest >> (shift - 1)) & 0x01;                                    \
    ror(dest, dest, shift);                                                   \
  }                                                                           \

#define thumb_shift_lsl_imm()                                                 \
  u32 dest = reg[rs];                                                         \
  if(imm != 0)                                                                \
  {                                                                           \
    c_flag = (dest >> (32 - imm)) & 0x01;                                     \
    dest <<= imm;                                                             \
  }                                                                           \

#define thumb_shift_lsr_imm()                                                 \
  u32 dest;                                                                   \
  if(imm == 0)                                                                \
  {                                                                           \
    dest = 0;                                                                 \
    c_flag = reg[rs] >> 31;                                                   \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = reg[rs];                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    dest >>= imm;                                                             \
  }                                                                           \

#define thumb_shift_asr_imm()                                                 \
  u32 dest;                                                                   \
  if(imm == 0)                                                                \
  {                                                                           \
    dest = (s32)reg[rs] >> 31;                                                \
    c_flag = dest & 0x01;                                                     \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    dest = reg[rs];                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    dest = (s32)dest >> imm;                                                  \
  }                                                                           \

#define thumb_shift_ror_imm()                                                 \
  u32 dest = reg[rs];                                                         \
  if(imm == 0)                                                                \
  {                                                                           \
    u32 old_c_flag = c_flag;                                                  \
    c_flag = dest & 0x01;                                                     \
    dest = (dest >> 1) | (old_c_flag << 31);                                  \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    c_flag = (dest >> (imm - 1)) & 0x01;                                      \
    ror(dest, dest, imm);                                                     \
  }                                                                           \

#define thumb_shift(decode_type, op_type, value_type)                         \
{                                                                             \
  thumb_decode_##decode_type();                                               \
  thumb_shift_##op_type##_##value_type();                                     \
  calculate_flags_logic(dest);                                                \
  reg[rd] = dest;                                                             \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_add(type, src_a, src_b)                                    \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa + _sb;                                                       \
  c_flag = (dest < _sb);                                                      \
  calculate_flags_add(dest, src_a, src_b);                                    \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_sub(type, src_a, src_b)                                    \
{                                                                             \
  thumb_decode_##type();                                                      \
  const u32 _sa = src_a;                                                      \
  const u32 _sb = src_b;                                                      \
  u32 dest = _sa - _sb;                                                       \
  calculate_flags_sub(dest, src_a, src_b, 1);                                 \
  thumb_pc_offset(2);                                                         \
}                                                                             \

#define thumb_test_logic(type, expr)                                          \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 dest = expr;                                                            \
  calculate_flags_logic(dest);                                                \
  thumb_pc_offset(2);                                                         \
}

#define thumb_hireg_op(expr)                                                  \
{                                                                             \
  thumb_pc_offset(4);                                                         \
  thumb_decode_hireg_op();                                                    \
  u32 dest = expr;                                                            \
  thumb_pc_offset(-2);                                                        \
  if(rd == 15)                                                                \
  {                                                                           \
    reg[REG_PC] = dest & ~0x01;                                               \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    reg[rd] = dest;                                                           \
  }                                                                           \
}                                                                             \

// Operation types: imm, mem_reg, mem_imm

#define thumb_access_memory(access_type, op_type, address, reg_op,            \
 mem_type)                                                                    \
{                                                                             \
  thumb_pc_offset(2);                                                         \
  thumb_decode_##op_type();                                                   \
  access_type##_memory_##mem_type(address, reg_op);                           \
}                                                                             \

#define thumb_conditional_branch(condition)                                   \
{                                                                             \
  thumb_decode_branch_cond();                                                 \
  if(condition)                                                               \
  {                                                                           \
    thumb_pc_offset((offset * 2) + 4);                                        \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    thumb_pc_offset(2);                                                       \
  }                                                                           \
  cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];                      \
}                                                                             \

// When a mode change occurs from non-FIQ to non-FIQ retire the current
// reg[13] and reg[14] into reg_mode[cpu_mode][5] and reg_mode[cpu_mode][6]
// respectively and load into reg[13] and reg[14] reg_mode[new_mode][5] and
// reg_mode[new_mode][6]. When swapping to/from FIQ retire/load reg[8]
// through reg[14] to/from reg_mode[MODE_FIQ][0] through reg_mode[MODE_FIQ][6].

const u32 cpu_modes[16] =
{
  MODE_USER, MODE_FIQ, MODE_IRQ, MODE_SUPERVISOR,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_ABORT,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_UNDEFINED,
  MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_SYSTEM
};

// ARM/Thumb mode is stored in the flags directly, this is simpler than
// shadowing it since it has a constant 1bit represenation.

u32 instruction_count = 0;

void set_cpu_mode(cpu_mode_type new_mode)
{
  cpu_mode_type cpu_mode = reg[CPU_MODE];

  if(cpu_mode == new_mode)
     return;

  if(new_mode == MODE_FIQ)
  {
     for (u32 i = 8; i < 15; i++)
        REG_MODE(cpu_mode)[i - 8] = reg[i];
  }
  else
  {
     REG_MODE(cpu_mode)[5] = reg[REG_SP];
     REG_MODE(cpu_mode)[6] = reg[REG_LR];
  }

  if(cpu_mode == MODE_FIQ)
  {
     for (u32 i = 8; i < 15; i++)
        reg[i] = REG_MODE(new_mode)[i - 8];
  }
  else
  {
     reg[REG_SP] = REG_MODE(new_mode)[5];
     reg[REG_LR] = REG_MODE(new_mode)[6];
  }

  reg[CPU_MODE] = new_mode;
}

#define cpu_has_interrupt()                                 \
  (!(reg[REG_CPSR] & 0x80) && read_ioreg(REG_IME) &&        \
    (read_ioreg(REG_IE) & read_ioreg(REG_IF)))

// Returns whether the CPU has a pending interrupt.
cpu_alert_type check_interrupt() {
  return (cpu_has_interrupt()) ? CPU_ALERT_IRQ : CPU_ALERT_NONE;
}

// Checks for pending IRQs and raises them. This changes the CPU mode
// which means that it must be called with a valid CPU state.
u32 check_and_raise_interrupts()
{
  // Check any IRQ flag pending, IME and CPSR-IRQ enabled
  if (cpu_has_interrupt())
  {
    // Value after the FIQ returns, should be improved
    reg[REG_BUS_VALUE] = 0xe55ec002;

    // Interrupt handler in BIOS
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
    reg[REG_CPSR] = 0xD2;
    reg[REG_PC] = 0x00000018;

    set_cpu_mode(MODE_IRQ);

    // Wake up CPU if it is stopped/sleeping.
    if (reg[CPU_HALT_STATE] == CPU_STOP ||
        reg[CPU_HALT_STATE] == CPU_HALT)
      reg[CPU_HALT_STATE] = CPU_ACTIVE;

    return 1;
  }
  return 0;
}

// This function marks a pending interrupt but does not raise it.
// It simply updates IF register and returns whether the IRQ needs
// to be raised (that is, IE/IME/CPSR enable the IRQ).
// Safe to call via dynarec without proper registers saved.
cpu_alert_type flag_interrupt(irq_type irq_raised)
{
  // Flag interrupt
  write_ioreg(REG_IF, read_ioreg(REG_IF) | irq_raised);

  return check_interrupt();
}

#ifndef HAVE_DYNAREC

// When switching modes set spsr[new_mode] to cpsr. Modifying PC as the
// target of a data proc instruction will set cpsr to spsr[cpu_mode].
u32 reg[64];
u32 spsr[6];
u32 reg_mode[7][7];

u16 oam_ram[512];
u16 palette_ram[512];
u16 palette_ram_converted[512];
#ifndef RETRO_GO
u8 ewram[(1024 * 256) << SMC_DETECTION];
u8 iwram[(1024 * 32) << SMC_DETECTION];
u8 vram[1024 * 96];
#endif
u8 *memory_map_read[8 * 1024];
u16 io_registers[512];
#endif

// Seam for the Xtensa JIT (cpu_jit.c): with budget>0, execute_arm returns
// control at the first ROM Thumb instruction after (budget-1) executed Thumb
// instructions, instead of running the whole frame. Costs one predictable test
// per Thumb instruction when disabled (budget=0). (The JIT uses the same seam.)
u32 cached_exit_budget = 0;
u32 cached_exit_flag = 0;
s32 cached_exit_cycles = 0;

/* m4a-HLE: high-level emulation of the GBA MP2k (m4a) sound mixer.
 *  (a) SoundInfo is found via the MP2k magic 0x68736D54 ("Tmsh") with a magic
 *      scan over IWRAM+EWRAM;
 *  (b) the mixer entry is determined empirically via rising edge (entering the hot
 *      mixer range); the entry PC that most often carried r0->SoundInfo = the real
 *      mixer entry. After M4A_LOCK hits it is locked as the hook.
 * With Audio=Off the mixer is skipped (CPU saving); the native mixer (M4A_A2)
 * replaces the inner mix loop per channel. M4A_DETECT 0 = zero overhead. */
#define M4A_DETECT 1                 /* 1 = detection + hook; 0 = zero overhead */
#if M4A_DETECT
#include <stdio.h>
#define M4A_A2         1             /* native mixer: skip inner mix loop per channel, advance position, jump to writeback */
#define M4A_A2_VERIFY  0             /* 1 = native alongside real + byte-compare; 0 = native-only */
#define M4A_MAGIC      0x68736D54u
#define M4A_NHIST      24
#define M4A_LOCK       3u            /* this many r0-OK on one PC → lock as hook address */
#define M4A_FORCE_SKIP 0             /* 1 = ignore Audio toggle, always skip */
#define M4A_DIAG       0             /* 1 = periodic channel-status/envelope dump during HLE skip */
#define M4A_RUN_EVERY  1             /* 0 = always HLE-skip, 1 = never skip, N = every Nth */
#define M4A_WRMAP      0             /* 1=SoundInfo window, 2=whole IWRAM(PSRAM)+IO registers */
#define M4A_DUMP_IWRAM 0             /* 1 = write live IWRAM (32KB) to SD on lock */
#define M4A_REVERB_HLE 1             /* HLE the MP2k reverb inner loop (post-process pcmBuffer); 0 = interpreter */
extern "C" { u32 m4a_entry_pc; u32 m4a_entry_lr; u32 m4a_soundinfo_ptr; }
static u32 m4a_hist_pc[M4A_NHIST];
static u32 m4a_hist_cnt[M4A_NHIST];   /* entries at this PC */
static u32 m4a_hist_r0[M4A_NHIST];    /* of which with r0 -> valid SoundInfo */
static u32 m4a_hist_n, m4a_hist_over, m4a_entries, m4a_prev_in;
static u32 m4a_hook_pc, m4a_hooked, m4a_skips, m4a_last_se = 2;   /* skip-hook state */
static u32 m4a_ch_hook, m4a_wb;                                   /* A2-1: channel entry hook + writeback PC (derived from entry) */
static u32 m4a_reverb_pc;                                         /* reverb inner-loop PC (signature) → HLE hook */
#if M4A_A2
static u32 m4a_vf_done, m4a_vf_pending, m4a_vf_base, m4a_vf_words; /* A2-2: verify state */
static u32 m4a_vf_R[64], m4a_vf_L[64];                            /* native result (verify scratch) */
static u32 m4a_vf_count, m4a_vf_cp, m4a_vf_fw;                    /* native end state */
#endif

/* Read u32/u8 at a GBA address from the IWRAM/EWRAM arrays (where SoundInfo lives).
 * IWRAM: the live copy is at offset 0x8000*SMC_DETECTION (the first 32KB is the
 * SMC double buffer). Host (S3) and GBA are both little-endian → direct read. */
static inline u32 m4a_read32(u32 addr)
{
  if (((addr >> 24) & 0xF) == 3 && (addr & 0xFFFFFF) < 0x8000)
    return *(u32 *)(iwram + (addr & 0x7FFF) + (0x8000 * SMC_DETECTION));
  if (((addr >> 24) & 0xF) == 2 && (addr & 0xFFFFFF) < 0x40000)
    return *(u32 *)(ewram + (addr & 0x3FFFF));
  return 0;
}
static inline u8 m4a_read8(u32 addr)
{
  if (((addr >> 24) & 0xF) == 3 && (addr & 0xFFFFFF) < 0x8000)
    return iwram[(addr & 0x7FFF) + (0x8000 * SMC_DETECTION)];
  if (((addr >> 24) & 0xF) == 2 && (addr & 0xFFFFFF) < 0x40000)
    return ewram[addr & 0x3FFFF];
  return 0;
}
static inline void m4a_write8(u32 addr, u8 v)
{
  if (((addr >> 24) & 0xF) == 3 && (addr & 0xFFFFFF) < 0x8000)
    iwram[(addr & 0x7FFF) + (0x8000 * SMC_DETECTION)] = v;
  else if (((addr >> 24) & 0xF) == 2 && (addr & 0xFFFFFF) < 0x40000)
    ewram[addr & 0x3FFFF] = v;
}
static inline void m4a_write32(u32 addr, u32 v)
{
  if (((addr >> 24) & 0xF) == 3 && (addr & 0xFFFFFF) < 0x8000)
    *(u32 *)(iwram + (addr & 0x7FFF) + (0x8000 * SMC_DETECTION)) = v;
  else if (((addr >> 24) & 0xF) == 2 && (addr & 0xFFFFFF) < 0x40000)
    *(u32 *)(ewram + (addr & 0x3FFFF)) = v;
}
static u32 m4a_scan_magic(void)   /* independent confirmation */
{
  u32 i;
  for (i = 0; i + 4 <= 0x8000; i += 4)
    if (*(u32 *)(iwram + i + (0x8000 * SMC_DETECTION)) == M4A_MAGIC)
      return 0x03000000u + i;
  for (i = 0; i + 4 <= 0x40000; i += 4)
    if (*(u32 *)(ewram + i) == M4A_MAGIC)
      return 0x02000000u + i;
  return 0;
}
/* A2-3: search for a byte signature in the live IWRAM (32KB). The MP2k mixer machine
 * code is identical across games → inner loop / channel entry / writeback have fixed
 * bytes, independent of the mixer address (which differs per game). Returns the GBA
 * address or 0. */
static u32 m4a_scan_sig(const u8 *sig, u32 len)
{
  u8 *base = iwram + (0x8000 * SMC_DETECTION);
  u32 i, j;
  for (i = 0; i + len <= 0x8000; i++) {
    for (j = 0; j < len; j++)
      if (base[i + j] != sig[j]) break;
    if (j == len) return 0x03000000u + i;
  }
  return 0;
}
/* MP2k SoundMainRAM signatures (ARM, LE):
 * channel entry = `str r8,[sp]; ldr r9,[r4,#1c]; ldrb r10,[r4,#a]; ldrb r11,[r4,#b]` → hook = +4;
 * writeback     = `str r9,[r4,#1c]; str r2,[r4,#18]; str r3,[r4,#28]` → jump target. */
static const u8 m4a_sig_chan[16] = {0x00,0x80,0x8d,0xe5, 0x1c,0x90,0x94,0xe5,
                                    0x0a,0xa0,0xd4,0xe5, 0x0b,0xb0,0xd4,0xe5};
#if M4A_REVERB_HLE
/* Reverb inner loop (ARM, 0x..ac4): ldrsb r0,[r5,r6]; ldrsb r1,[r5]; add r0,r0,r1;
 * ldrsb r1,[r7,r6] — distinctive, version-independent. */
static const u8 m4a_sig_reverb[16] = {0xd6,0x00,0x95,0xe1, 0xd0,0x10,0xd5,0xe1,
                                      0x01,0x00,0x80,0xe0, 0xd6,0x10,0x97,0xe1};
/* Native MP2k reverb: byte-exact replica of the inner loop (0x..ac4..0x..afc). Per sample:
 * average of 4 bytes (cur L at [r5+r6], cur R at [r5], reverb tap [r7+r6]/[r7]),
 * * coeff(r3) >> 9, rounding (bit7), back to pcmBuffer. Regs at loop entry:
 * r3=coeff r4=count r5=pcmBuf r6=L→R offset r7=reverb tap. */
static void m4a_reverb_native(void)
{
  s32 cnt = (s32)reg[4], coeff = (s32)reg[3];
  u32 r5 = reg[5], r6 = reg[6], r7 = reg[7];
  while (cnt > 0) {
    s32 s = (s8)m4a_read8(r5 + r6) + (s8)m4a_read8(r5)
          + (s8)m4a_read8(r7 + r6) + (s8)m4a_read8(r7);
    r7++;
    s32 v = (s * coeff) >> 9;
    if (v & 0x80) v++;                       /* tst r0,#0x80; addne r0,#1 */
    m4a_write8(r5 + r6, (u8)v);
    m4a_write8(r5, (u8)v);
    r5++; cnt--;
  }
  reg[4] = 0; reg[5] = r5; reg[7] = r7;      /* post-loop (downstream reloads; for tidiness) */
}
#endif
static const u8 m4a_sig_wb[12]   = {0x1c,0x90,0x84,0xe5, 0x18,0x20,0x84,0xe5, 0x28,0x30,0x84,0xe5};
static void m4a_dump(void)
{
  u32 i, c, best = 0, bestr0 = 0;
  printf("[M4A] ================ detection ================\n");
  printf("[M4A] IWRAM entries with r0->SoundInfo (n=%u, overflow=%u, total=%u):\n",
         m4a_hist_n, m4a_hist_over, m4a_entries);
  for (i = 0; i < m4a_hist_n; i++) {
    if (m4a_hist_r0[i] > bestr0) { bestr0 = m4a_hist_r0[i]; best = m4a_hist_pc[i]; }
  }
  for (i = 0; i < m4a_hist_n; i++)
    printf("[M4A]   PC=0x%08x  entries=%u  r0->SoundInfo=%u%s\n",
           m4a_hist_pc[i], m4a_hist_cnt[i], m4a_hist_r0[i],
           m4a_hist_pc[i] == best && bestr0 ? "  <-- SoundMainRAM entry" : "");
  m4a_entry_pc = best;
  printf("[M4A] SoundMainRAM entry (most r0-OK) = 0x%08x  LR(skip target)=0x%08x\n",
         best, m4a_entry_lr);

  u32 scan = m4a_scan_magic();
  u32 sr0 = m4a_soundinfo_ptr;   /* r0 of the confirmed SoundMainRAM entry */
  printf("[M4A] r0-SoundInfo=0x%08x (magic=0x%08x %s)  scan-found=0x%08x\n",
         sr0, m4a_read32(sr0), m4a_read32(sr0) == M4A_MAGIC ? "OK" : "BAD", scan);
  u32 si = (m4a_read32(sr0) == M4A_MAGIC) ? sr0 : scan;   /* else scan result */
  m4a_soundinfo_ptr = si;
  if (si && m4a_read32(si) == M4A_MAGIC) {
    printf("[M4A] SoundInfo @ 0x%08x:\n", si);
    printf("[M4A]   ident=0x%08x pcmDmaCounter=%u maxChans=%u masterVolume=%u\n",
           m4a_read32(si), m4a_read8(si + 0x04), m4a_read8(si + 0x06), m4a_read8(si + 0x07));
    printf("[M4A]   pcmSamplesPerVBlank=%d pcmFreq=%d divFreq=%d\n",
           (s32)m4a_read32(si + 0x10), (s32)m4a_read32(si + 0x14), (s32)m4a_read32(si + 0x18));
    printf("[M4A]   cgbChans=0x%08x MPlayHead=0x%08x musPlrHead=0x%08x CgbSound=0x%08x\n",
           m4a_read32(si + 0x1C), m4a_read32(si + 0x20), m4a_read32(si + 0x24), m4a_read32(si + 0x28));
    for (c = 0; c < 8; c++) {
      u32 ch = si + 0x50 + c * 0x40;
      printf("[M4A]   chan[%u] status=0x%02x type=0x%02x L=%u R=%u freq=0x%08x wav=0x%08x\n",
             c, m4a_read8(ch + 0x00), m4a_read8(ch + 0x01), m4a_read8(ch + 0x03),
             m4a_read8(ch + 0x02), m4a_read32(ch + 0x20), m4a_read32(ch + 0x24));
    }
  } else {
    printf("[M4A] No valid SoundInfo found (audio not initialised yet?).\n");
  }
#if M4A_DUMP_IWRAM
  /* Dump live IWRAM (32KB) → SD. Contains the SoundMainRAM copied into IWRAM
   * (THUMB entry @ m4a_entry_pc → ARM body + inner-mixloop variants), for offline
   * disassembly to derive the inner-loop entry/exit PC, register convention and
   * byte signature for the generic scanner. */
  {
    FILE *df = fopen("/sd/retro-go/m4a_iwram.bin", "wb");
    if (df) {
      fwrite(iwram + (0x8000 * SMC_DETECTION), 1, 0x8000, df);
      fclose(df);
      printf("[M4A] IWRAM (32KB) dumped -> /sd/retro-go/m4a_iwram.bin"
             " (entry=0x%08x, base 0x03000000)\n", m4a_entry_pc);
    } else {
      printf("[M4A] DUMP FAILED (fopen /sd/retro-go/m4a_iwram.bin)\n");
    }
  }
#endif
  printf("[M4A] ===================================================\n");
}
static void m4a_entry(u32 pc, u32 r0, u32 lr)
{
  u32 i;
  if (m4a_read32(r0) != M4A_MAGIC) return;   /* only mixer-related entries (r0->SoundInfo) */
  m4a_soundinfo_ptr = r0; m4a_entry_lr = lr;
  m4a_entries++;
  for (i = 0; i < m4a_hist_n; i++)
    if (m4a_hist_pc[i] == pc) { m4a_hist_cnt[i]++; m4a_hist_r0[i]++; goto counted; }
  if (m4a_hist_n < M4A_NHIST) {
    m4a_hist_pc[m4a_hist_n] = pc; m4a_hist_cnt[m4a_hist_n] = 1;
    m4a_hist_r0[m4a_hist_n] = 1; i = m4a_hist_n++;
  } else { m4a_hist_over++; return; }
counted:
  /* Lock once the same entry PC repeatedly carried r0->SoundInfo = SoundMainRAM.
   * (LR may be odd: SoundMainRAM is called from Thumb.) */
  if (m4a_hist_r0[i] >= M4A_LOCK) {
    m4a_hook_pc = pc; m4a_entry_pc = pc; m4a_hooked = 1;
#if M4A_A2
    /* A2-3: find the channel entry + writeback via byte signature (version-independent),
     * instead of fixed offsets from the entry (which differ per MP2k version). */
    {
      u32 chan = m4a_scan_sig(m4a_sig_chan, sizeof(m4a_sig_chan));
      u32 wb   = m4a_scan_sig(m4a_sig_wb, sizeof(m4a_sig_wb));
      if (chan && wb) {
        m4a_ch_hook = chan + 4;          /* hook after `str r8,[sp]` */
        m4a_wb      = wb;                /* writeback (str fw/count/cp) */
        printf("[M4A] A2-3: signatures OK  chan-entry=0x%08x hook=0x%08x wb=0x%08x\n",
               chan, m4a_ch_hook, m4a_wb);
      } else {
        m4a_ch_hook = 0; m4a_wb = 0;     /* not found → A2-1 inactive = safe fallback */
        printf("[M4A] A2-3: signature NOT found (chan=0x%08x wb=0x%08x) → A2-1 off (interpreter)\n",
               chan, wb);
      }
    }
#endif
#if M4A_REVERB_HLE
    m4a_reverb_pc = m4a_scan_sig(m4a_sig_reverb, sizeof(m4a_sig_reverb));
    printf("[M4A] reverb-HLE: loop-PC=0x%08x %s\n", m4a_reverb_pc,
           m4a_reverb_pc ? "(active)" : "(not found -> interpreter)");
#endif
    m4a_dump();
    printf("[M4A] skip-hook ARMED at 0x%08x (sound_master_enable=%u, FORCE_SKIP=%u)\n",
           pc, (u32)sound_master_enable, (u32)M4A_FORCE_SKIP);
  }
}
/* Native MP2k envelope/status bookkeeping (replaces SoundMainRAM when Audio=Off).
 * Replays the per-channel envelope state machine so the game-visible status advances
 * correctly (note-finished = status->0); skips the expensive per-sample mix loop.
 * Based on NBA mp2k.cc + pokeemerald m4a_internal.h. Offsets in SoundChannel (0x40 bytes):
 * +00 status +02 rVol +03 lVol +04 attack +05 decay +06 sustain +07 release
 * +09 envVol +0A envVolR +0B envVolL +0C echoVol +0D echoLen. */
#define M4A_SF_START   0x80u
#define M4A_SF_STOP    0x40u
#define M4A_SF_IEC     0x04u   /* ECHO actief */
#define M4A_SF_ENV     0x03u
#define M4A_SF_ATTACK  0x03u
#define M4A_SF_DECAY   0x02u
#define M4A_SF_SUSTAIN 0x01u
static void __attribute__((unused)) m4a_hle_bookkeeping(u32 si)
{
  u32 c, maxChans = m4a_read8(si + 0x06);
  u32 spv = (u32)m4a_read32(si + 0x10);        /* pcmSamplesPerVBlank */
  u32 pcmFreq = (u32)m4a_read32(si + 0x14);
  if (maxChans > 12) maxChans = 12;
  if (pcmFreq == 0) pcmFreq = 1;
  for (c = 0; c < maxChans; c++) {
    u32 ch = si + 0x50u + c * 0x40u;
    u32 st = m4a_read8(ch + 0x00);
    u32 env = m4a_read8(ch + 0x09);
    if (st == 0) continue;                              /* inactief */

    if (st & M4A_SF_START) {                            /* new note */
      if (st & M4A_SF_STOP) { m4a_write8(ch + 0x00, 0); continue; }
      env = m4a_read8(ch + 0x04);                       /* env = attack */
      /* START cleared, env state set, other flags (LOOP/IEC) preserved */
      st = (st & ~(M4A_SF_START | M4A_SF_ENV)) |
           ((env == 0xFF) ? M4A_SF_DECAY : M4A_SF_ATTACK);
      /* initialise sample position like the real mixer does on START */
      { u32 wav = m4a_read32(ch + 0x24);
        if (wav) { m4a_write32(ch + 0x28, wav + 0x10);          /* currentPointer = &wav->data */
                   m4a_write32(ch + 0x18, m4a_read32(wav + 0x0C)); /* count = wav->size */
                   m4a_write32(ch + 0x1C, 0); } }                /* fw = 0 */
    } else if (st & M4A_SF_STOP) {                      /* note-off → RELEASE/ECHO */
      if (st & M4A_SF_IEC) {                            /* in ECHO: count down */
        u32 el = m4a_read8(ch + 0x0D);
        if (el == 0) { m4a_write8(ch + 0x00, 0); continue; }   /* note done */
        m4a_write8(ch + 0x0D, (u8)(el - 1));
      } else {                                          /* RELEASE */
        env = (env * m4a_read8(ch + 0x07)) >> 8;        /* *release */
        u32 echo = m4a_read8(ch + 0x0C);
        if (env <= echo) {
          if (echo == 0) { m4a_write8(ch + 0x00, 0); continue; }  /* note done */
          env = echo; st |= M4A_SF_IEC;                 /* switch to ECHO */
        }
      }
    } else {
      switch (st & M4A_SF_ENV) {
        case M4A_SF_ATTACK:
          env += m4a_read8(ch + 0x04);
          if (env > 0xFE) { env = 0xFF; st = (st & ~M4A_SF_ENV) | M4A_SF_DECAY; }
          break;
        case M4A_SF_DECAY: {
          u32 sus = m4a_read8(ch + 0x06);
          env = (env * m4a_read8(ch + 0x05)) >> 8;      /* *decay */
          if (env <= sus) { env = sus; st = (st & ~M4A_SF_ENV) | M4A_SF_SUSTAIN; }
          break;
        }
        default: break;                                 /* SUSTAIN: hold */
      }
    }
    m4a_write8(ch + 0x09, (u8)env);
    m4a_write8(ch + 0x00, (u8)st);
    /* envelopeVolumeR/L: raw scaling (only relevant for real audio). */
    m4a_write8(ch + 0x0A, (u8)((env * m4a_read8(ch + 0x02)) >> 8));
    m4a_write8(ch + 0x0B, (u8)((env * m4a_read8(ch + 0x03)) >> 8));

    /* Advance the sample position in BULK (no per-sample loop): adv=freq*spv/pcmFreq
     * samples. Update currentPointer/count; loop-wrap or clamp at the end. This is the
     * per-frame side effect the game engine needs to free channels (else it hangs). */
    {
      u32 freq = m4a_read32(ch + 0x20);
      u32 wav  = m4a_read32(ch + 0x24);
      if (freq && wav) {
        u32 adv = (u32)(((u64)freq * spv) / pcmFreq);
        s32 cnt = (s32)m4a_read32(ch + 0x18) - (s32)adv;
        u32 cp  = m4a_read32(ch + 0x28) + adv;
        if (cnt <= 0) {
          if (st & 0x10u) {                              /* LOOP flag → wrap */
            u32 sz = m4a_read32(wav + 0x0C), ls = m4a_read32(wav + 0x08);
            s32 ll = (s32)(sz - ls);
            if (ll > 0) { while (cnt <= 0) { cnt += ll; cp -= (u32)ll; } }
            else cnt = 0;
          } else {
            cnt = 0;                                     /* non-loop: clamp at end */
          }
        }
        m4a_write32(ch + 0x18, (u32)cnt);
        m4a_write32(ch + 0x28, cp);
      }
    }
  }
#if M4A_DIAG
  /* Diagnostics: every ~200 frames dump channel status + envelopeVolume + pcmDmaCounter
   * to see whether the envelope resolves (status->0) or stays stuck. */
  static u32 dctr = 0;
  if ((dctr++ % 200) == 0) {
    u32 cc; char line[200]; int p = 0;
    p += sprintf(line + p, "[M4A] dma=%u st:", m4a_read8(si + 0x04));
    for (cc = 0; cc < maxChans; cc++) {
      u32 chh = si + 0x50u + cc * 0x40u;
      p += sprintf(line + p, " %02x/%02x", m4a_read8(chh + 0x00), m4a_read8(chh + 0x09));
    }
    printf("%s\n", line);
  }
#endif
}

/* True if we may skip this instruction (the mixer entry): hook armed, Audio=Off,
 * PC at the hook address and r0 verifies the magic. The mode-correct return
 * (BX LR, ARM or Thumb) happens at the call site. */
#if M4A_RUN_EVERY != 0
static u32 m4a_callcnt;
#endif
#if M4A_A2
/* A2-1: skip the inner per-sample mix loop of ONE channel. Hook at the channel ARM
 * entry (`str r8,[sp]` just ran): r4=SoundChannel, r2=count, r3=currentPointer already
 * loaded. We advance the position natively (adv=freq*spv/pcmFreq), write reg[2/3/9],
 * and the caller jumps to the writeback (pop {r4,r12}; str fw/count/cp; bx THUMB). The
 * real outer mixer code (status/CGB/globals) keeps running.
 * Audio Off only; pcmBuffer is not filled (audio may be dropped). No wave-loop wrap in
 * v1 (count clamps) — drift, no hang. Return 0 = not safe → let the real mixer run. */
static int m4a_a2_skip_channel(void)
{
  u32 ch = reg[4];
  if (((ch >> 24) & 0xF) != 3u) return 0;            /* r4 must be a SoundChannel in IWRAM */
  u32 si = m4a_soundinfo_ptr;
  if (!si || m4a_read32(si) != M4A_MAGIC) return 0;
  u32 spv     = (u32)m4a_read32(si + 0x10);          /* pcmSamplesPerVBlank */
  u32 pcmFreq = (u32)m4a_read32(si + 0x14); if (!pcmFreq) pcmFreq = 1;
  u32 freq    = m4a_read32(ch + 0x20);               /* SoundChannel.frequency */
  u32 count   = reg[2];                              /* nog te spelen samples */
  u32 cp      = reg[3];                              /* currentPointer */
  u32 adv     = (u32)(((u64)freq * (u64)spv) / pcmFreq);
  if (adv > count) adv = count;                      /* v1: no loop-wrap → clamp */
  reg[2] = count - adv;                              /* count */
  reg[3] = cp + adv;                                 /* currentPointer */
  reg[9] = m4a_read32(ch + 0x1c);                    /* fw unchanged (v1) */
  return 1;
}
#if M4A_A2
/* read_memory8 (gba_memory.h) reads ROM/IWRAM for the wave samples. */
static u32 m4a_rom32(u32 a)
{ return read_memory8(a) | (read_memory8(a+1)<<8) | (read_memory8(a+2)<<16) | ((u32)read_memory8(a+3)<<24); }

/* A2-2: native MP2k resampling mixer (linear interpolation + packed accumulator), byte-exact
 * validated against a real channel capture on hardware (56/56 words + count/fw exact; cp=+1 =
 * the real `sub r3,#1`). Phase step r4=divFreq*freq; interpolate per output sample, 4 samples/word
 * in r6/r7 (ror#8), R at base, L at base+0x630. flip=0 (verify): fills scratch m4a_vf_R/L. flip=1:
 * fills pcmBuffer + sets reg[2/3/9] (count/cp-1/fw) so the real writeback stores them → the caller
 * jumps to the writeback. Resampling channels only (type bits 0x38 empty); else 0 (fall back). */
static int m4a_a2_native_impl(int flip)
{
  u32 ch = reg[4], si = m4a_soundinfo_ptr;
  if (((ch >> 24) & 0xF) != 3u) return 0;
  if (!si || m4a_read32(si) != M4A_MAGIC) return 0;
  u8 ctype = m4a_read8(ch + 1);
  if (ctype & 0x30u) return 0;                        /* special channels → interpreter (fall back) */
  int simple = (ctype & 0x08u) != 0;                  /* A2-3: simple loop (1:1, no interpolation) */
  u32 r8 = reg[8], words = r8 >> 2;
  if (words == 0 || words > 64) return 0;
  u32 freq = m4a_read32(ch + 0x20), divFreq = (u32)m4a_read32(si + 0x18);
  u32 r4 = (u32)(divFreq * freq);
  u32 r10 = (u32)m4a_read8(ch + 0x0a) << 16, r11 = (u32)m4a_read8(ch + 0x0b) << 16;
  u32 fw = m4a_read32(ch + 0x1c);
  s32 count = (s32)reg[2];
  u32 cp = reg[3];
  u32 wav = m4a_read32(ch + 0x24), wdata = wav + 0x10;
  s32 loopStart = (s32)m4a_rom32(wav + 0x08), wsize = (s32)m4a_rom32(wav + 0x0C);
  u32 base = reg[5] & 0x3FFFFFFFu;

  if (simple) {
    /* A2-3: native simple loop (1:1, no phase/interpolation). Accumulation identical to
     * resampling (mul envVolR/L; bic ~0xff0000; add ror#8). Per output: cur=ldrsb[r3],#1;
     * count-- with wave restart (cp=loopStart, count+=loopLen). Does NOT touch fw (reg[9]=fw
     * unchanged) and stores cp WITHOUT -1 (the real simple loop jumps to writeback+4, after the
     * `sub r3,#1`). */
    s32 loopLen = wsize - loopStart;
    for (u32 w = 0; w < words; w++) {
      u32 r6 = m4a_read32(base + w*4), r7 = m4a_read32(base + 0x630 + w*4);
      for (int k = 0; k < 4; k++) {
        s32 cur = (s8)read_memory8(cp); cp++;          /* ldrsb r0,[r3],#1 */
        u32 ip = ((u32)(r10 * (u32)cur)) & ~0x00ff0000u; r6 = ip + ((r6 >> 8) | (r6 << 24));
        ip = ((u32)(r11 * (u32)cur)) & ~0x00ff0000u;     r7 = ip + ((r7 >> 8) | (r7 << 24));
        if (--count <= 0) { cp = wdata + (u32)loopStart; count += loopLen; }   /* wave restart */
      }
      if (flip) { m4a_write32(base + 0x630 + w*4, r7); m4a_write32(base + w*4, r6); }
      else      { m4a_vf_R[w] = r6; m4a_vf_L[w] = r7; }
    }
    if (flip) { reg[2] = (u32)count; reg[3] = cp; reg[9] = fw; }   /* fw unchanged; cp WITHOUT -1 */
    else { m4a_vf_base = base; m4a_vf_words = words; m4a_vf_pending = 1;
           m4a_vf_count = (u32)count; m4a_vf_cp = cp; m4a_vf_fw = fw; }
    return 1;
  }

  s32 r0 = (s8)read_memory8(cp); cp++;                /* ldrsb r0,[r3]; ldrsb r1,[r3,#1]!; sub */
  s32 r1 = (s32)(s8)read_memory8(cp) - r0;
  for (u32 w = 0; w < words; w++) {
    u32 r6 = m4a_read32(base + w*4), r7 = m4a_read32(base + 0x630 + w*4);  /* accu before this channel */
    for (int k = 0; k < 4; k++) {
      s32 lr = (s32)(u32)(fw * (u32)r1);              /* mul lr,r9,delta */
      lr = r0 + (lr >> 23);                           /* add lr,r0,lr asr#23  (interpolation) */
      u32 ip = ((u32)(r10 * (u32)lr)) & ~0x00ff0000u; r6 = ip + ((r6 >> 8) | (r6 << 24));
      ip = ((u32)(r11 * (u32)lr)) & ~0x00ff0000u;     r7 = ip + ((r7 >> 8) | (r7 << 24));
      fw = fw + r4;
      u32 step = fw >> 23;
      if (step) {
        fw &= ~0x3F800000u;
        count -= (s32)step;
        if (count <= 0) {                             /* wave restart (loop) */
          cp = wdata + (u32)loopStart; count += (wsize - loopStart);
          r0 = (s8)read_memory8(cp); cp++; r1 = (s32)(s8)read_memory8(cp) - r0;
        } else {
          step -= 1;
          if (step == 0) r0 = r0 + r1;                /* single-step: current += delta */
          else { cp += step; r0 = (s8)read_memory8(cp); }
          cp++; r1 = (s32)(s8)read_memory8(cp) - r0;
        }
      }
    }
    if (flip) { m4a_write32(base + 0x630 + w*4, r7); m4a_write32(base + w*4, r6); }
    else      { m4a_vf_R[w] = r6; m4a_vf_L[w] = r7; }
  }
  if (flip) {
    reg[2] = (u32)count; reg[3] = cp - 1; reg[9] = fw;        /* cp-1 = the real `sub r3,#1` */
  } else {
    m4a_vf_base = base; m4a_vf_words = words; m4a_vf_pending = 1;
    m4a_vf_count = (u32)count; m4a_vf_cp = cp; m4a_vf_fw = fw;
  }
  return 1;
}
static inline int m4a_a2_native(int flip)
{
  return m4a_a2_native_impl(flip);
}
#endif
#if M4A_A2_VERIFY
/* Compare the native scratch with what the real mixer wrote to pcmBuffer (one-shot, first channel). */
static void m4a_a2_verify_cmp(void)
{
  u32 w, diffs = 0, first = 0xffffffffu;
  for (w = 0; w < m4a_vf_words; w++) {
    u32 rr = (u32)m4a_read32(m4a_vf_base + w*4), rl = (u32)m4a_read32(m4a_vf_base + 0x630 + w*4);
    if (rr != m4a_vf_R[w] || rl != m4a_vf_L[w]) { if (first == 0xffffffffu) first = w; diffs++; }
  }
  printf("[A2VF] pcm: words=%u mismatch=%u (0=BYTE-EXACT) | state native vs real: "
         "count %u/%u cp %08x/%08x fw %08x/%08x\n",
         m4a_vf_words, diffs, m4a_vf_count, reg[2], m4a_vf_cp, reg[3], m4a_vf_fw, reg[9]);
  if (diffs) {
    w = first;
    printf("[A2VF]  first@w%u: R native=%08x real=%08x | L native=%08x real=%08x\n", w,
           (unsigned)m4a_vf_R[w], (unsigned)m4a_read32(m4a_vf_base + w*4),
           (unsigned)m4a_vf_L[w], (unsigned)m4a_read32(m4a_vf_base + 0x630 + w*4));
  }
  m4a_vf_pending = 0; m4a_vf_done = 1;
}
#endif
#endif
static inline u32 __attribute__((unused)) m4a_want_skip(void)
{
#if M4A_FORCE_SKIP
  u32 audio_off = 1u;
#else
  u32 audio_off = !sound_master_enable;
#endif
  if (!(m4a_hooked && audio_off && reg[REG_PC] == m4a_hook_pc &&
        m4a_read32(reg[0]) == M4A_MAGIC))
    return 0;
#if M4A_RUN_EVERY == 0
  return 1;                                   /* always HLE-skip (real mixer never) */
#else
  return (++m4a_callcnt % M4A_RUN_EVERY) != 0;/* real mixer every Nth call */
#endif
}
#if M4A_WRMAP
/* Write mapping: snapshot an IWRAM window around SoundInfo at the mixer entry, and
 * diff it once the mixer leaves IWRAM (= return) → shows which bytes (outside
 * pcmBuffer) the real mixer writes per frame. One-shot. M4A_RUN_EVERY=1 (no skip)
 * so the real mixer runs and the game doesn't hang. */
#define M4A_SNAP_SZ 0x800u                       /* 2KB window: SoundInfo header + 12 channels */
static u8  m4a_snap[M4A_SNAP_SZ];
static u32 m4a_snap_active, m4a_wrmap_done, m4a_snap_base;
#if M4A_WRMAP >= 2
static u8 *m4a_isnap;                             /* 32KB IWRAM snapshot in PSRAM */
static u16 m4a_iosnap[512];                       /* IO registers snapshot */
#endif
static void m4a_wrmap_diff(u32 si)
{
  u32 i, rs = 0, inrun = 0;
  u32 pcm_lo = si + 0x50u + 12u * 0x40u;       /* pcmBuffer (rel. GBA address) */
  u32 pcm_hi = pcm_lo + 1584u * 2u;
  printf("[M4A] === write-map (1 frame): changed IWRAM bytes outside pcmBuffer ===\n");
  for (i = 0; i < M4A_SNAP_SZ; i++) {
    u32 gba = 0x03000000u + m4a_snap_base + i;
    u8  now = iwram[m4a_snap_base + i + (0x8000 * SMC_DETECTION)];
    u32 chg = (now != m4a_snap[i]) && !(gba >= pcm_lo && gba < pcm_hi);
    if (chg && !inrun) { rs = gba; inrun = 1; }
    else if (!chg && inrun) {
      printf("[M4A]   0x%08x..0x%08x  (si%+d..si%+d)\n", rs, gba - 1,
             (s32)(rs - si), (s32)(gba - 1 - si));
      inrun = 0;
    }
  }
  if (inrun) printf("[M4A]   ..0x%08x (to window edge)\n", 0x03000000u + m4a_snap_base + M4A_SNAP_SZ - 1);
#if M4A_WRMAP >= 2
  /* Diff the whole IWRAM + IO registers to find writes outside the SoundInfo window. */
  {
    extern u16 io_registers[512];
    u32 j, irs = 0, iinrun = 0;
    if (m4a_isnap) {
      printf("[M4A] -- whole IWRAM (outside pcmBuffer + outside SoundInfo window) --\n");
      for (j = 0; j < 0x8000; j++) {
        u32 gba = 0x03000000u + j;
        u8 now = iwram[j + (0x8000 * SMC_DETECTION)];
        u32 inwin = (j >= m4a_snap_base && j < m4a_snap_base + M4A_SNAP_SZ);
        u32 chg = (now != m4a_isnap[j]) && !(gba >= pcm_lo && gba < pcm_hi) && !inwin;
        if (chg && !iinrun) { irs = gba; iinrun = 1; }
        else if (!chg && iinrun) { printf("[M4A]   IW 0x%08x..0x%08x\n", irs, gba-1); iinrun = 0; }
      }
      if (iinrun) printf("[M4A]   IW 0x%08x..tail\n", irs);
    }
    printf("[M4A] -- IO registers (0x040000xx) changed --\n");
    for (j = 0; j < 512; j++)
      if (io_registers[j] != m4a_iosnap[j])
        printf("[M4A]   IO 0x%08x: 0x%04x->0x%04x\n", 0x04000000u + j*2u, m4a_iosnap[j], io_registers[j]);
  }
#endif
  /* Per channel the before(snapshot)->after(live) of the position fields, to derive the
   * exact advance formula (count/fw/currentPointer as a function of frequency, spv). */
  {
    u32 spv = (u32)m4a_read32(si + 0x10);   /* pcmSamplesPerVBlank */
    u32 mc = m4a_read8(si + 0x06), c;
    if (mc > 12) mc = 12;
    printf("[M4A] spv=%u, per channel  freq | count b->a | fw b->a | cp b->a:\n", spv);
    for (c = 0; c < mc; c++) {
      u32 ch = si + 0x50u + c * 0x40u;
      u32 bi = (ch & 0x7FFFu) - m4a_snap_base;     /* snapshot index of channel start */
      if (bi + 0x40 > M4A_SNAP_SZ) continue;       /* outside window */
      #define SNB(o) ((u32)m4a_snap[bi+(o)] | (m4a_snap[bi+(o)+1]<<8) | (m4a_snap[bi+(o)+2]<<16) | ((u32)m4a_snap[bi+(o)+3]<<24))
      printf("[M4A]  ch%u f=0x%08x cnt %u->%u fw 0x%08x->0x%08x cp 0x%08x->0x%08x\n",
             c, m4a_read32(ch + 0x20),
             SNB(0x18), m4a_read32(ch + 0x18),
             SNB(0x1C), m4a_read32(ch + 0x1C),
             SNB(0x28), m4a_read32(ch + 0x28));
      #undef SNB
    }
  }
  printf("[M4A] === write-map done ===\n");
}
#endif
static void m4a_post_hook(void)
{
  if ((u32)sound_master_enable != m4a_last_se) {
    m4a_last_se = sound_master_enable;
    printf("[M4A] Audio state changed: sound_master_enable=%u\n", m4a_last_se);
  }
#if M4A_WRMAP
  if (!m4a_wrmap_done && m4a_soundinfo_ptr) {
    u32 pc = reg[REG_PC], si = m4a_soundinfo_ptr;
    if (!m4a_snap_active && pc == m4a_hook_pc) {
      m4a_snap_base = (si & 0x7FFFu) & ~(M4A_SNAP_SZ - 1u);  /* aligned window */
      memcpy(m4a_snap, iwram + m4a_snap_base + (0x8000 * SMC_DETECTION), M4A_SNAP_SZ);
#if M4A_WRMAP >= 2
      extern u16 io_registers[512];
      if (!m4a_isnap) m4a_isnap = (u8 *)heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM);
      if (m4a_isnap) memcpy(m4a_isnap, iwram + (0x8000 * SMC_DETECTION), 0x8000);
      memcpy(m4a_iosnap, io_registers, sizeof(m4a_iosnap));
#endif
      m4a_snap_active = 1;
    } else if (m4a_snap_active && (pc >> 24) != 3u) {     /* mixer left IWRAM = return */
      m4a_wrmap_diff(si);
      m4a_snap_active = 0; m4a_wrmap_done = 1;
    }
  }
#endif
}
/* Per interpreted instruction: detect rising edge into the mixer range.
 * Runs until the hook is locked. */
#define M4A_PROBE() do {                                                       \
  if (!m4a_hooked) {                                                           \
    u32 _pc = reg[REG_PC];                                                     \
    u32 _in = ((_pc >> 24) == 3u);   /* IWRAM region, address-generic */       \
    if (_in && !m4a_prev_in) m4a_entry(_pc, reg[0], reg[REG_LR]);             \
    m4a_prev_in = _in;                                                         \
  } else {                                                                     \
    m4a_post_hook();                                                           \
  }                                                                            \
} while (0)
#else
#define M4A_PROBE() do { } while (0)
#endif

/* SWI-HLE: intercept BIOS CpuSet (0x0B) + CpuFastSet (0x0C) — memcpy/fill SWIs that
 * Pokémon calls heavily per frame (OAM/palette/tiles). Does the transfer natively
 * via write_memory* so the side effects on VRAM/palette/OAM are identical to the real
 * BIOS = byte-exact. r0=src
 * r1=dst r2=ctrl (bits0-20=count, b24=fill, b26=32bit for CpuSet). Returns 1 = handled.
 * Kill-switch SWI_HLE 0 = real BIOS. */
#define SWI_HLE 1
#if SWI_HLE
static int swi_hle(u32 num, cpu_alert_type *alert)
{
  u32 src = reg[0], dst = reg[1], ctrl = reg[2];
  u32 count = ctrl & 0x1FFFFFu;
  if (num == 0x0Bu) {                                  /* CpuSet */
    int fill = (ctrl >> 24) & 1;
    if (ctrl & 0x04000000u) {                          /* 32-bit */
      src &= ~3u; dst &= ~3u;
      u32 v = fill ? read_memory32(src) : 0;
      for (u32 i = 0; i < count; i++) {
        if (!fill) { v = read_memory32(src); src += 4; }
        *alert |= write_memory32(dst, v); dst += 4;
      }
    } else {                                           /* 16-bit */
      src &= ~1u; dst &= ~1u;
      u32 v = fill ? read_memory16(src) : 0;
      for (u32 i = 0; i < count; i++) {
        if (!fill) { v = read_memory16(src); src += 2; }
        *alert |= write_memory16(dst, (u16)v); dst += 2;
      }
    }
    return 1;
  }
  if (num == 0x0Cu) {                                  /* CpuFastSet (32-bit, veelvoud van 8) */
    int fill = (ctrl >> 24) & 1;
    src &= ~3u; dst &= ~3u;
    count = (count + 7u) & ~7u;
    u32 v = fill ? read_memory32(src) : 0;
    for (u32 i = 0; i < count; i++) {
      if (!fill) { v = read_memory32(src); src += 4; }
      *alert |= write_memory32(dst, v); dst += 4;
    }
    return 1;
  }
  /* LZ77UnCompWram (0x11) / LZ77UnCompVram (0x12) — the tileset/tilemap decompression
   * used on e.g. building enter/exit. Interpreted through the ARM BIOS this costs
   * ~130ms per tileset switch; native it is ~1ms. Strategy: decompress into a temp
   * buffer (back-refs always valid), then bulk-write so the side effects (VRAM dirty
   * marking) stay identical to the real BIOS. Non-standard input (no type-1 header,
   * implausible size, odd VRAM dst, back-ref before start, alloc failure) → return 0
   * = real BIOS path, behaviour unchanged. */
  if (num == 0x11u || num == 0x12u) {
    u32 header = read_memory32(src);
    u32 dsize = header >> 8;
    if (((header >> 4) & 0xFu) != 1u || dsize == 0 || dsize > 0x100000u)
      return 0;
    if (num == 0x12u && (dst & 1u))
      return 0;
    u8 *tmp = (u8 *)malloc(dsize + 1);           /* +1: pad for the last halfword write */
    if (!tmp)
      return 0;
    u32 s = src + 4, w = 0;
    while (w < dsize) {
      u32 flags = read_memory8(s); s++;
      for (int b = 0; b < 8 && w < dsize; b++, flags <<= 1) {
        if (flags & 0x80u) {
          u32 c0 = read_memory8(s); s++;
          u32 c1 = read_memory8(s); s++;
          u32 n = (c0 >> 4) + 3;
          u32 disp = (((c0 & 0xFu) << 8) | c1) + 1;
          if (disp > w) { free(tmp); return 0; }  /* back-ref before start → BIOS path */
          while (n-- && w < dsize) { tmp[w] = tmp[w - disp]; w++; }
        } else {
          tmp[w++] = (u8)read_memory8(s); s++;
        }
      }
    }
    tmp[dsize] = 0;
    u32 blen = (num == 0x12u) ? ((dsize + 1u) & ~1u) : dsize;
    if (!gba_bulk_write(dst, tmp, blen, num == 0x12u)) {
      if (num == 0x11u) {
        for (u32 i = 0; i < dsize; i++)
          *alert |= write_memory8(dst + i, tmp[i]);
      } else {
        for (u32 i = 0; i < dsize; i += 2)
          *alert |= write_memory16(dst + i, (u16)(tmp[i] | (tmp[i + 1] << 8)));
      }
    }
    free(tmp);
    return 1;
  }
  return 0;
}
#endif

IRAM_ATTR void execute_arm(u32 cycles)
{
  u32 opcode;
  u32 condition;
  u32 n_flag, z_flag, c_flag, v_flag;
  u32 pc_region = (reg[REG_PC] >> 15);
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 new_pc_region;
  s32 cycles_remaining;
  u32 update_ret;
  cpu_alert_type cpu_alert;

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);
  touch_gamepak_page(pc_region);

  cycles_remaining = cycles;
  while(1)
  {
    /* Do not execute until CPU is active */
    if (reg[CPU_HALT_STATE] != CPU_ACTIVE) {
       u32 ret = update_gba(cycles_remaining);
       if (completed_frame(ret))
          return;

       cycles_remaining = cycles_to_run(ret);
    }

    cpu_alert = CPU_ALERT_NONE;
    extract_flags();

    if(reg[REG_CPSR] & 0x20)
      goto thumb_loop;

    do
    {
arm_loop:

       M4A_PROBE();
#if M4A_A2
#if M4A_A2_VERIFY
       /* A2-2 it.2: native mixer alongside the real one (Audio ON, no skip) + byte-compare. One-shot. */
       if (m4a_hooked && !m4a_vf_done && sound_master_enable) {
          /* A2-3 verify: only verify simple-loop channels (type&0x08); resampling is already byte-exact. */
          if (!m4a_vf_pending && reg[REG_PC] == m4a_ch_hook && (m4a_read8(reg[4] + 1) & 0x08u))
                                                                   m4a_a2_native(0);
          /* resampling writeback = m4a_wb (0x..e0); simple loop jumps to +4 (skips the fw store) */
          else if (m4a_vf_pending && (reg[REG_PC] == m4a_wb || reg[REG_PC] == m4a_wb + 4))
                                                                   m4a_a2_verify_cmp();
       }
#else
       /* A2-2 FLIP: Audio ON → mix resampling channels natively (fill pcmBuffer) + skip the real
        * inner loop → speed with sound. Non-resampling channels (return 0) run the real mixer. */
       if (m4a_hooked && sound_master_enable && reg[REG_PC] == m4a_ch_hook && m4a_a2_native(1)) {
          reg[REG_PC] = m4a_wb;
          goto arm_loop;
       }
#endif
#if M4A_REVERB_HLE
       /* Reverb-HLE: replace the ARM reverb inner loop with the native version and jump to
        * the loop exit (loop-PC + 0x3c = `add r0,pc; bx` → back to Thumb). Output goes to
        * the audio DMA, not game state → a bug only affects sound, no hang. */
       if (m4a_hooked && m4a_reverb_pc && reg[REG_PC] == m4a_reverb_pc) {
          m4a_reverb_native();
          reg[REG_PC] = m4a_reverb_pc + 0x3c;
          goto arm_loop;
       }
#endif
       /* A2-1: skip only the inner mix loop per channel (real outer mixer keeps running),
        * advance position natively, jump to the writeback. Audio Off. */
       if (m4a_hooked && !sound_master_enable && reg[REG_PC] == m4a_ch_hook
           && m4a_a2_skip_channel()) {
          if (!m4a_skips++)
             printf("[M4A] A2-1: inner-skip ACTIVE, chan-hook=0x%08x -> writeback=0x%08x (Audio=Off)\n",
                    m4a_ch_hook, m4a_wb);
          reg[REG_PC] = m4a_wb;        /* pop {r4,r12}; str fw/count/cp; bx THUMB */
          goto arm_loop;
       }
#elif M4A_DETECT
       /* Full-skip fallback — only when A2 is off. */
       if (m4a_want_skip()) {
          u32 _lr = reg[REG_LR];
          m4a_hle_bookkeeping(reg[0]);
          if (!m4a_skips++)
             printf("[M4A] HLE-skip ACTIEF op 0x%08x -> LR=0x%08x (Audio=Off, bookkeeping)\n",
                    m4a_hook_pc, _lr);
          if (_lr & 1u) { reg[REG_PC] = _lr - 1; reg[REG_CPSR] |= 0x20; goto thumb_loop; }
          reg[REG_PC] = _lr;
          goto arm_loop;
       }
#endif
       collapse_flags();

       /* Process cheats if we are about to execute the cheat hook */
       if (reg[REG_PC] == cheat_master_hook)
          process_cheats();

       /* Execute ARM instruction */
       using_instruction(arm);
       check_pc_region();
       reg[REG_PC] &= ~0x03;
       opcode = readaddress32(pc_address_block, (reg[REG_PC] & 0x7FFF));
       condition = opcode >> 28;

       switch(condition)
       {
          case 0x0:
             /* EQ */
             if(!z_flag)
                arm_next_instruction();
             break;
          case 0x1:
             /* NE      */
             if(z_flag)
                arm_next_instruction();
             break;
          case 0x2:
             /* CS       */
             if(!c_flag)
                arm_next_instruction();
             break;
          case 0x3:
             /* CC       */
             if(c_flag)
                arm_next_instruction();
             break;
          case 0x4:
             /* MI       */
             if(!n_flag)
                arm_next_instruction();
             break;

          case 0x5:
             /* PL       */
             if(n_flag)
                arm_next_instruction();
             break;

          case 0x6:
             /* VS       */
             if(!v_flag)
                arm_next_instruction();
             break;

          case 0x7:
             /* VC       */
             if(v_flag)
                arm_next_instruction();
             break;

          case 0x8:
             /* HI       */
             if((c_flag == 0) | z_flag)
                arm_next_instruction();
             break;

          case 0x9:
             /* LS       */
             if(c_flag & (z_flag ^ 1))
                arm_next_instruction();
             break;

          case 0xA:
             /* GE       */
             if(n_flag != v_flag)
                arm_next_instruction();
             break;

          case 0xB:
             /* LT       */
             if(n_flag == v_flag)
                arm_next_instruction();
             break;

          case 0xC:
             /* GT       */
             if(z_flag | (n_flag != v_flag))
                arm_next_instruction();
             break;

          case 0xD:
             /* LE       */
             if((z_flag == 0) & (n_flag == v_flag))
                arm_next_instruction();
             break;

          case 0xE:
             /* AL       */
             break;

          case 0xF:
             /* Reserved - treat as "never" */
             arm_next_instruction();
             break;
       }

       #ifdef TRACE_INSTRUCTIONS
       interp_trace_instruction(reg[REG_PC], 1);
       #endif

       switch((opcode >> 20) & 0xFF)
       {
          case 0x00:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], -rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, - reg[rm]);
                }
                else
                {
                   /* MUL rd, rm, rs */
                   arm_multiply(no_op, no);
                }
             }
             else
             {
                /* AND rd, rn, reg_op */
                arm_data_proc(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x01:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* MULS rd, rm, rs */
                      arm_multiply(no_op, yes);
                      break;

                   case 1:
                      /* LDRH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, - reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, - reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, - reg[rm]);
                      break;
                }
             }
             else
             {
                /* ANDS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x02:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], -rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, - reg[rm]);
                }
                else
                {
                   /* MLA rd, rm, rs, rn */
                   arm_multiply(+ reg[rn], no);
                }
             }
             else
             {
                /* EOR rd, rn, reg_op */
                arm_data_proc(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x03:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* MLAS rd, rm, rs, rn */
                      arm_multiply(+ reg[rn], yes);
                      break;

                   case 1:
                      /* LDRH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, - reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, - reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, - reg[rm]);
                      break;
                }
             }
             else
             {
                /* EORS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x04:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn], -imm */
                arm_access_memory(store, no_op, half_imm, u16, yes, - offset);
             }
             else
             {
                /* SUB rd, rn, reg_op */
                arm_data_proc(reg[rn] - reg_sh, reg);
             }
             break;

          case 0x05:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, - offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, - offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, - offset);
                      break;
                }
             }
             else
             {
                /* SUBS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg[rn], reg_sh, 1, reg);
             }
             break;

          case 0x06:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn], -imm */
                arm_access_memory(store, no_op, half_imm, u16, yes, - offset);
             }
             else
             {
                /* RSB rd, rn, reg_op */
                arm_data_proc(reg_sh - reg[rn], reg);
             }
             break;

          case 0x07:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, - offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, - offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], -imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, - offset);
                      break;
                }
             }
             else
             {
                /* RSBS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg_sh, reg[rn], 1, reg);
             }
             break;

          case 0x08:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, + reg[rm]);
                }
                else
                {
                   /* UMULL rd, rm, rs */
                   arm_multiply_long(no_op, no, u);
                }
             }
             else
             {
                /* ADD rd, rn, reg_op */
                arm_data_proc(reg[rn] + reg_sh, reg);
             }
             break;

          case 0x09:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* UMULLS rdlo, rdhi, rm, rs */
                      arm_multiply_long(no_op, yes, u);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, + reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, + reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, + reg[rm]);
                      break;
                }
             }
             else
             {
                /* ADDS rd, rn, reg_op */
                arm_data_proc_add_flags(reg[rn], reg_sh, 0, reg);
             }
             break;

          case 0x0A:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +rm */
                   arm_access_memory(store, no_op, half_reg, u16, yes, + reg[rm]);
                }
                else
                {
                   /* UMLAL rd, rm, rs */
                   arm_multiply_long(arm_multiply_long_addop(u), no, u);
                }
             }
             else
             {
                /* ADC rd, rn, reg_op */
                arm_data_proc(reg[rn] + reg_sh + c_flag, reg);
             }
             break;

          case 0x0B:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* UMLALS rdlo, rdhi, rm, rs */
                      arm_multiply_long(arm_multiply_long_addop(u), yes, u);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, u16, yes, + reg[rm]);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s8, yes, + reg[rm]);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +rm */
                      arm_access_memory(load, no_op, half_reg, s16, yes, + reg[rm]);
                      break;
                }
             }
             else
             {
                /* ADCS rd, rn, reg_op */
                arm_data_proc_add_flags(reg[rn], reg_sh, c_flag, reg);
             }
             break;

          case 0x0C:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +imm */
                   arm_access_memory(store, no_op, half_imm, u16, yes, + offset);
                }
                else
                {
                   /* SMULL rd, rm, rs */
                   arm_multiply_long(no_op, no, s);
                }
             }
             else
             {
                /* SBC rd, rn, reg_op */
                arm_data_proc(reg[rn] - (reg_sh + (c_flag ^ 1)), reg);
             }
             break;

          case 0x0D:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* SMULLS rdlo, rdhi, rm, rs */
                      arm_multiply_long(no_op, yes, s);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, + offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, + offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, + offset);
                      break;
                }
             }
             else
             {
                /* SBCS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg[rn], reg_sh, c_flag, reg);
             }
             break;

          case 0x0E:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn], +imm */
                   arm_access_memory(store, no_op, half_imm, u16, yes, + offset);
                }
                else
                {
                   /* SMLAL rd, rm, rs */
                   arm_multiply_long(arm_multiply_long_addop(s), no, s);
                }
             }
             else
             {
                /* RSC rd, rn, reg_op */
                arm_data_proc(reg_sh - reg[rn] + c_flag - 1, reg);
             }
             break;

          case 0x0F:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 0:
                      /* SMLALS rdlo, rdhi, rm, rs */
                      arm_multiply_long(arm_multiply_long_addop(s), yes, s);
                      break;

                   case 1:
                      /* LDRH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, u16, yes, + offset);
                      break;

                   case 2:
                      /* LDRSB rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s8, yes, + offset);
                      break;

                   case 3:
                      /* LDRSH rd, [rn], +imm */
                      arm_access_memory(load, no_op, half_imm, s16, yes, + offset);
                      break;
                }
             }
             else
             {
                /* RSCS rd, rn, reg_op */
                arm_data_proc_sub_flags(reg_sh, reg[rn], c_flag, reg);
             }
             break;

          case 0x10:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn - rm] */
                   arm_access_memory(store, - reg[rm], half_reg, u16, no, no_op);
                }
                else
                {
                   /* SWP rd, rm, [rn] */
                   arm_swap(u32);
                }
             }
             else
             {
                /* MRS rd, cpsr */
                arm_psr(reg, read, reg[REG_CPSR]);
             }
             break;

          case 0x11:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - rm] */
                      arm_access_memory(load, - reg[rm], half_reg, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* TST rd, rn, reg_op */
                arm_data_proc_test_logic(reg[rn] & reg_sh, reg);
             }
             break;

          case 0x12:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn - rm]! */
                arm_access_memory(store, - reg[rm], half_reg, u16, yes, no_op);
             }
             else
             {
                if(opcode & 0x10)
                {
                   /* BX rn */
                   arm_decode_branchx(opcode);
                   u32 src = reg[rn];
                   if(src & 0x01)
                   {
                      reg[REG_PC] = src - 1;
                      reg[REG_CPSR] |= 0x20;
                      goto thumb_loop;
                   }
                   else
                   {
                      reg[REG_PC] = src;
                   }
                   cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                }
                else
                {
                   /* MSR cpsr, rm */
                   arm_psr(reg, store, cpsr);
                }
             }
             break;

          case 0x13:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - rm]! */
                      arm_access_memory(load, - reg[rm], half_reg, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* TEQ rd, rn, reg_op */
                arm_data_proc_test_logic(reg[rn] ^ reg_sh, reg);
             }
             break;

          case 0x14:
             if((opcode & 0x90) == 0x90)
             {
                if(opcode & 0x20)
                {
                   /* STRH rd, [rn - imm] */
                   arm_access_memory(store, - offset, half_imm, u16, no, no_op);
                }
                else
                {
                   /* SWPB rd, rm, [rn] */
                   arm_swap(u8);
                }
             }
             else
             {
                /* MRS rd, spsr */
                arm_psr(reg, read, REG_SPSR(reg[CPU_MODE]));
             }
             break;

          case 0x15:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - imm] */
                      arm_access_memory(load, - offset, half_imm, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* CMP rn, reg_op */
                arm_data_proc_test_sub(reg[rn], reg_sh, reg);
             }
             break;

          case 0x16:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn - imm]! */
                arm_access_memory(store, - offset, half_imm, u16, yes, no_op);
             }
             else
             {
                /* MSR spsr, rm */
                arm_psr(reg, store, spsr);
             }
             break;

          case 0x17:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn - imm]! */
                      arm_access_memory(load, - offset, half_imm, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* CMN rd, rn, reg_op */
                arm_data_proc_test_add(reg[rn], reg_sh, reg);
             }
             break;

          case 0x18:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + rm] */
                arm_access_memory(store, + reg[rm], half_reg, u16, no, no_op);
             }
             else
             {
                /* ORR rd, rn, reg_op */
                arm_data_proc(reg[rn] | reg_sh, reg);
             }
             break;

          case 0x19:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + rm] */
                      arm_access_memory(load, + reg[rm], half_reg, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* ORRS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] | reg_sh, reg);
             }
             break;

          case 0x1A:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + rm]! */
                arm_access_memory(store, + reg[rm], half_reg, u16, yes, no_op);
             }
             else
             {
                /* MOV rd, reg_op */
                arm_data_proc(reg_sh, reg);
             }
             break;

          case 0x1B:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + rm]! */
                      arm_access_memory(load, + reg[rm], half_reg, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* MOVS rd, reg_op */
                arm_data_proc_logic_flags(reg_sh, reg);
             }
             break;

          case 0x1C:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + imm] */
                arm_access_memory(store, + offset, half_imm, u16, no, no_op);
             }
             else
             {
                /* BIC rd, rn, reg_op */
                arm_data_proc(reg[rn] & (~reg_sh), reg);
             }
             break;

          case 0x1D:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, u16, no, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, s8, no, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + imm] */
                      arm_access_memory(load, + offset, half_imm, s16, no, no_op);
                      break;
                }
             }
             else
             {
                /* BICS rd, rn, reg_op */
                arm_data_proc_logic_flags(reg[rn] & (~reg_sh), reg);
             }
             break;

          case 0x1E:
             if((opcode & 0x90) == 0x90)
             {
                /* STRH rd, [rn + imm]! */
                arm_access_memory(store, + offset, half_imm, u16, yes, no_op);
             }
             else
             {
                /* MVN rd, reg_op */
                arm_data_proc(~reg_sh, reg);
             }
             break;

          case 0x1F:
             if((opcode & 0x90) == 0x90)
             {
                switch((opcode >> 5) & 0x03)
                {
                   case 1:
                      /* LDRH rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, u16, yes, no_op);
                      break;

                   case 2:
                      /* LDRSB rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, s8, yes, no_op);
                      break;

                   case 3:
                      /* LDRSH rd, [rn + imm]! */
                      arm_access_memory(load, + offset, half_imm, s16, yes, no_op);
                      break;
                }
             }
             else
             {
                /* MVNS rd, rn, reg_op */
                arm_data_proc_logic_flags(~reg_sh, reg);
             }
             break;

          case 0x20:
             /* AND rd, rn, imm */
             arm_data_proc(reg[rn] & imm, imm);
             break;

          case 0x21:
             /* ANDS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] & imm, imm);
             break;

          case 0x22:
             /* EOR rd, rn, imm */
             arm_data_proc(reg[rn] ^ imm, imm);
             break;

          case 0x23:
             /* EORS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] ^ imm, imm);
             break;

          case 0x24:
             /* SUB rd, rn, imm */
             arm_data_proc(reg[rn] - imm, imm);
             break;

          case 0x25:
             /* SUBS rd, rn, imm */
             arm_data_proc_sub_flags(reg[rn], imm, 1, imm);
             break;

          case 0x26:
             /* RSB rd, rn, imm */
             arm_data_proc(imm - reg[rn], imm);
             break;

          case 0x27:
             /* RSBS rd, rn, imm */
             arm_data_proc_sub_flags(imm, reg[rn], 1, imm);
             break;

          case 0x28:
             /* ADD rd, rn, imm */
             arm_data_proc(reg[rn] + imm, imm);
             break;

          case 0x29:
             /* ADDS rd, rn, imm */
             arm_data_proc_add_flags(reg[rn], imm, 0, imm);
             break;

          case 0x2A:
             /* ADC rd, rn, imm */
             arm_data_proc(reg[rn] + imm + c_flag, imm);
             break;

          case 0x2B:
             /* ADCS rd, rn, imm */
             arm_data_proc_add_flags(reg[rn], imm, c_flag, imm);
             break;

          case 0x2C:
             /* SBC rd, rn, imm */
             arm_data_proc(reg[rn] - imm + c_flag - 1, imm);
             break;

          case 0x2D:
             /* SBCS rd, rn, imm */
             arm_data_proc_sub_flags(reg[rn], imm, c_flag, imm);
             break;

          case 0x2E:
             /* RSC rd, rn, imm */
             arm_data_proc(imm - reg[rn] + c_flag - 1, imm);
             break;

          case 0x2F:
             /* RSCS rd, rn, imm */
             arm_data_proc_sub_flags(imm, reg[rn], c_flag, imm);
             break;

          case 0x30:
          case 0x31:
             /* TST rn, imm */
             arm_data_proc_test_logic(reg[rn] & imm, imm);
             break;

          case 0x32:
             /* MSR cpsr, imm */
             arm_psr(imm, store, cpsr);
             break;

          case 0x33:
             /* TEQ rn, imm */
             arm_data_proc_test_logic(reg[rn] ^ imm, imm);
             break;

          case 0x34:
          case 0x35:
             /* CMP rn, imm */
             arm_data_proc_test_sub(reg[rn], imm, imm);
             break;

          case 0x36:
             /* MSR spsr, imm */
             arm_psr(imm, store, spsr);
             break;

          case 0x37:
             /* CMN rn, imm */
             arm_data_proc_test_add(reg[rn], imm, imm);
             break;

          case 0x38:
             /* ORR rd, rn, imm */
             arm_data_proc(reg[rn] | imm, imm);
             break;

          case 0x39:
             /* ORRS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] | imm, imm);
             break;

          case 0x3A:
             /* MOV rd, imm */
             arm_data_proc(imm, imm);
             break;

          case 0x3B:
             /* MOVS rd, imm */
             arm_data_proc_logic_flags(imm, imm);
             break;

          case 0x3C:
             /* BIC rd, rn, imm */
             arm_data_proc(reg[rn] & (~imm), imm);
             break;

          case 0x3D:
             /* BICS rd, rn, imm */
             arm_data_proc_logic_flags(reg[rn] & (~imm), imm);
             break;

          case 0x3E:
             /* MVN rd, imm */
             arm_data_proc(~imm, imm);
             break;

          case 0x3F:
             /* MVNS rd, imm */
             arm_data_proc_logic_flags(~imm, imm);
             break;

          case 0x40:
             /* STR rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u32, yes, - offset);
             break;

          case 0x41:
             /* LDR rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u32, yes, - offset);
             break;

          case 0x42:
             /* STRT rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u32, yes, - offset);
             break;

          case 0x43:
             /* LDRT rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u32, yes, - offset);
             break;

          case 0x44:
             /* STRB rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u8, yes, - offset);
             break;

          case 0x45:
             /* LDRB rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u8, yes, - offset);
             break;

          case 0x46:
             /* STRBT rd, [rn], -imm */
             arm_access_memory(store, no_op, imm, u8, yes, - offset);
             break;

          case 0x47:
             /* LDRBT rd, [rn], -imm */
             arm_access_memory(load, no_op, imm, u8, yes, - offset);
             break;

          case 0x48:
             /* STR rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u32, yes, + offset);
             break;

          case 0x49:
             /* LDR rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u32, yes, + offset);
             break;

          case 0x4A:
             /* STRT rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u32, yes, + offset);
             break;

          case 0x4B:
             /* LDRT rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u32, yes, + offset);
             break;

          case 0x4C:
             /* STRB rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u8, yes, + offset);
             break;

          case 0x4D:
             /* LDRB rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u8, yes, + offset);
             break;

          case 0x4E:
             /* STRBT rd, [rn], +imm */
             arm_access_memory(store, no_op, imm, u8, yes, + offset);
             break;

          case 0x4F:
             /* LDRBT rd, [rn], +imm */
             arm_access_memory(load, no_op, imm, u8, yes, + offset);
             break;

          case 0x50:
             /* STR rd, [rn - imm] */
             arm_access_memory(store, - offset, imm, u32, no, no_op);
             break;

          case 0x51:
             /* LDR rd, [rn - imm] */
             arm_access_memory(load, - offset, imm, u32, no, no_op);
             break;

          case 0x52:
             /* STR rd, [rn - imm]! */
             arm_access_memory(store, - offset, imm, u32, yes, no_op);
             break;

          case 0x53:
             /* LDR rd, [rn - imm]! */
             arm_access_memory(load, - offset, imm, u32, yes, no_op);
             break;

          case 0x54:
             /* STRB rd, [rn - imm] */
             arm_access_memory(store, - offset, imm, u8, no, no_op);
             break;

          case 0x55:
             /* LDRB rd, [rn - imm] */
             arm_access_memory(load, - offset, imm, u8, no, no_op);
             break;

          case 0x56:
             /* STRB rd, [rn - imm]! */
             arm_access_memory(store, - offset, imm, u8, yes, no_op);
             break;

          case 0x57:
             /* LDRB rd, [rn - imm]! */
             arm_access_memory(load, - offset, imm, u8, yes, no_op);
             break;

          case 0x58:
             /* STR rd, [rn + imm] */
             arm_access_memory(store, + offset, imm, u32, no, no_op);
             break;

          case 0x59:
             /* LDR rd, [rn + imm] */
             arm_access_memory(load, + offset, imm, u32, no, no_op);
             break;

          case 0x5A:
             /* STR rd, [rn + imm]! */
             arm_access_memory(store, + offset, imm, u32, yes, no_op);
             break;

          case 0x5B:
             /* LDR rd, [rn + imm]! */
             arm_access_memory(load, + offset, imm, u32, yes, no_op);
             break;

          case 0x5C:
             /* STRB rd, [rn + imm] */
             arm_access_memory(store, + offset, imm, u8, no, no_op);
             break;

          case 0x5D:
             /* LDRB rd, [rn + imm] */
             arm_access_memory(load, + offset, imm, u8, no, no_op);
             break;

          case 0x5E:
             /* STRB rd, [rn + imm]! */
             arm_access_memory(store, + offset, imm, u8, yes, no_op);
             break;

          case 0x5F:
             /* LDRBT rd, [rn + imm]! */
             arm_access_memory(load, + offset, imm, u8, yes, no_op);
             break;

          case 0x60:
             /* STR rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x61:
             /* LDR rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x62:
             /* STRT rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x63:
             /* LDRT rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, - reg_offset);
             break;

          case 0x64:
             /* STRB rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x65:
             /* LDRB rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x66:
             /* STRBT rd, [rn], -reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x67:
             /* LDRBT rd, [rn], -reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, - reg_offset);
             break;

          case 0x68:
             /* STR rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x69:
             /* LDR rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6A:
             /* STRT rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6B:
             /* LDRT rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u32, yes, + reg_offset);
             break;

          case 0x6C:
             /* STRB rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6D:
             /* LDRB rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6E:
             /* STRBT rd, [rn], +reg_op */
             arm_access_memory(store, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x6F:
             /* LDRBT rd, [rn], +reg_op */
             arm_access_memory(load, no_op, reg, u8, yes, + reg_offset);
             break;

          case 0x70:
             /* STR rd, [rn - reg_op] */
             arm_access_memory(store, - reg_offset, reg, u32, no, no_op);
             break;

          case 0x71:
             /* LDR rd, [rn - reg_op] */
             arm_access_memory(load, - reg_offset, reg, u32, no, no_op);
             break;

          case 0x72:
             /* STR rd, [rn - reg_op]! */
             arm_access_memory(store, - reg_offset, reg, u32, yes, no_op);
             break;

          case 0x73:
             /* LDR rd, [rn - reg_op]! */
             arm_access_memory(load, - reg_offset, reg, u32, yes, no_op);
             break;

          case 0x74:
             /* STRB rd, [rn - reg_op] */
             arm_access_memory(store, - reg_offset, reg, u8, no, no_op);
             break;

          case 0x75:
             /* LDRB rd, [rn - reg_op] */
             arm_access_memory(load, - reg_offset, reg, u8, no, no_op);
             break;

          case 0x76:
             /* STRB rd, [rn - reg_op]! */
             arm_access_memory(store, - reg_offset, reg, u8, yes, no_op);
             break;

          case 0x77:
             /* LDRB rd, [rn - reg_op]! */
             arm_access_memory(load, - reg_offset, reg, u8, yes, no_op);
             break;

          case 0x78:
             /* STR rd, [rn + reg_op] */
             arm_access_memory(store, + reg_offset, reg, u32, no, no_op);
             break;

          case 0x79:
             /* LDR rd, [rn + reg_op] */
             arm_access_memory(load, + reg_offset, reg, u32, no, no_op);
             break;

          case 0x7A:
             /* STR rd, [rn + reg_op]! */
             arm_access_memory(store, + reg_offset, reg, u32, yes, no_op);
             break;

          case 0x7B:
             /* LDR rd, [rn + reg_op]! */
             arm_access_memory(load, + reg_offset, reg, u32, yes, no_op);
             break;

          case 0x7C:
             /* STRB rd, [rn + reg_op] */
             arm_access_memory(store, + reg_offset, reg, u8, no, no_op);
             break;

          case 0x7D:
             /* LDRB rd, [rn + reg_op] */
             arm_access_memory(load, + reg_offset, reg, u8, no, no_op);
             break;

          case 0x7E:
             /* STRB rd, [rn + reg_op]! */
             arm_access_memory(store, + reg_offset, reg, u8, yes, no_op);
             break;

          case 0x7F:
             /* LDRBT rd, [rn + reg_op]! */
             arm_access_memory(load, + reg_offset, reg, u8, yes, no_op);
             break;

          /* STM instructions: STMDA, STMIA, STMDB, STMIB */

          case 0x80:   /* STMDA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x88:   /* STMIA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x90:   /* STMDB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x98:   /* STMIB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, false, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x82:   /* STMDA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8A:   /* STMIA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x92:   /* STMDB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9A:   /* STMIB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccStore, true, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x84:   /* STMDA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8C:   /* STMIA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x94:   /* STMDB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9C:   /* STMIB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, false, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x86:   /* STMDA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8E:   /* STMIA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x96:   /* STMDB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9E:   /* STMIB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccStore, true, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;


          /* LDM instructions: LDMDA, LDMIA, LDMDB, LDMIB */

          case 0x81:   /* LDMDA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x89:   /* LDMIA rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x91:   /* LDMDB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x99:   /* LDMIB rn, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x83:   /* LDMDA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x8B:   /* LDMIA rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x93:   /* LDMDB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;
          case 0x9B:   /* LDMIB rn!, rlist */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, false, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            break;

          case 0x85:   /* LDMDA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x8D:   /* LDMIA rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x95:   /* LDMDB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x9D:   /* LDMIB rn, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, false, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;

          case 0x87:   /* LDMDA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPostDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x8F:   /* LDMIA rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPostInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x97:   /* LDMDB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPreDec>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;
          case 0x9F:   /* LDMIB rn!, rlist^ */
            cpu_alert |= exec_arm_block_mem<AccLoad, true, true, AddrPreInc>(
              (opcode >> 16) & 0x0F, opcode & 0xFFFF, cycles_remaining);
            arm_spsr_restore_ldm_check();
            break;


          case 0xA0 ... 0xAF:
             {
                /* B offset */
                arm_decode_branch();
                reg[REG_PC] += offset + 8;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                break;
             }

          case 0xB0 ... 0xBF:
             {
                /* BL offset */
                arm_decode_branch();
                reg[REG_LR] = reg[REG_PC] + 4;
                reg[REG_PC] += offset + 8;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][1];
                break;
             }

#ifdef HAVE_UNUSED
          case 0xC0 ... 0xEF:
             /* coprocessor instructions, reserved on GBA */
             break;
#endif

          case 0xF0 ... 0xFF:
#if SWI_HLE
            if (swi_hle((opcode >> 16) & 0xFFu, &cpu_alert)) {
               reg[REG_BUS_VALUE] = 0xe3a02004; reg[REG_PC] += 4; break;   /* native → volgende instr */
            }
#endif
            collapse_flags();
            reg[REG_BUS_VALUE] = 0xe3a02004;  // After SWI, we read bios[0xE4]
            REG_MODE(MODE_SUPERVISOR)[6] = reg[REG_PC] + 4;
            REG_SPSR(MODE_SUPERVISOR) = reg[REG_CPSR];
            reg[REG_PC] = 0x00000008;
            // Move to ARM mode, Supervisor mode and disable IRQs
            reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
            set_cpu_mode(MODE_SUPERVISOR);
            break;
       }

skip_instruction:

       /* End of Execute ARM instruction */
       cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][1];

       if (reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0) cycles_remaining = 0;

       if (cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
         goto alert;

    } while(cycles_remaining > 0);

    collapse_flags();
    update_ret = update_gba(cycles_remaining);
    if (completed_frame(update_ret))
       return;
    cycles_remaining = cycles_to_run(update_ret);
    continue;

    do
    {
thumb_loop:

       // JIT seam: return control at a ROM Thumb boundary (see above)
       if (cached_exit_budget && !--cached_exit_budget)
       {
          if ((u32)((reg[REG_PC] >> 24) - 0x08) <= 5) // PC in ROM (0x08-0x0D)
          {
             collapse_flags();
             cached_exit_flag = 1;
             cached_exit_cycles = cycles_remaining;
             return;
          }
          cached_exit_budget = 1; // retry on the next Thumb instruction
       }

       M4A_PROBE();
#if M4A_DETECT && !M4A_A2
       /* Full-skip fallback — only when A2 is off. A2-1 hooks the
        * channel entry in arm_loop (the mixer body is ARM), not here. */
       if (m4a_want_skip()) {
          u32 _lr = reg[REG_LR];
          m4a_hle_bookkeeping(reg[0]);
          if (!m4a_skips++)
             printf("[M4A] HLE-skip ACTIEF (thumb) op 0x%08x -> LR=0x%08x (Audio=Off, bookkeeping)\n",
                    m4a_hook_pc, _lr);
          if (_lr & 1u) { reg[REG_PC] = _lr - 1; reg[REG_CPSR] |= 0x20; goto thumb_loop; }
          reg[REG_PC] = _lr; reg[REG_CPSR] &= ~0x20u;
          goto arm_loop;
       }
#endif
       collapse_flags();

       /* Process cheats if we are about to execute the cheat hook */
       if (reg[REG_PC] == cheat_master_hook)
          process_cheats();

       /* Execute THUMB instruction */

       using_instruction(thumb);
       check_pc_region();
       reg[REG_PC] &= ~0x01;
       opcode = readaddress16(pc_address_block, (reg[REG_PC] & 0x7FFF));

       #ifdef TRACE_INSTRUCTIONS
       interp_trace_instruction(reg[REG_PC], 0);
       #endif

       switch((opcode >> 8) & 0xFF)
       {
          case 0x00 ... 0x07:
             /* LSL rd, rs, offset */
             thumb_shift(shift, lsl, imm);
             break;

          case 0x08 ... 0x0F:
             /* LSR rd, rs, offset */
             thumb_shift(shift, lsr, imm);
             break;

          case 0x10 ... 0x17:
             /* ASR rd, rs, offset */
             thumb_shift(shift, asr, imm);
             break;

          case 0x18:
          case 0x19:
             /* ADD rd, rs, rn */
             thumb_add(add_sub, rd, reg[rs], reg[rn], 0);
             break;

          case 0x1A:
          case 0x1B:
             /* SUB rd, rs, rn */
             thumb_sub(add_sub, rd, reg[rs], reg[rn], 1);
             break;

          case 0x1C:
          case 0x1D:
             /* ADD rd, rs, imm */
             thumb_add(add_sub_imm, rd, reg[rs], imm, 0);
             break;

          case 0x1E:
          case 0x1F:
             /* SUB rd, rs, imm */
             thumb_sub(add_sub_imm, rd, reg[rs], imm, 1);
             break;

          case 0x20 ... 0x27:
             /* MOV r0..7, imm */
             thumb_logic(imm, ((opcode >> 8) & 7), imm);
             break;

          case 0x28 ... 0x2F:
             /* CMP r0..7, imm */
             thumb_test_sub(imm, reg[(opcode >> 8) & 7], imm);
             break;

          case 0x30 ... 0x37:
             /* ADD r0..7, imm */
             thumb_add(imm, ((opcode >> 8) & 7), reg[(opcode >> 8) & 7], imm, 0);
             break;

          case 0x38 ... 0x3F:
             /* SUB r0..7, imm */
             thumb_sub(imm, ((opcode >> 8) & 7), reg[(opcode >> 8) & 7], imm, 1);
             break;

          case 0x40:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* AND rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] & reg[rs]);
                   break;

                case 0x01:
                   /* EOR rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] ^ reg[rs]);
                   break;

                case 0x02:
                   /* LSL rd, rs */
                   thumb_shift(alu_op, lsl, reg);
                   break;

                case 0x03:
                   /* LSR rd, rs */
                   thumb_shift(alu_op, lsr, reg);
                   break;
             }
             break;

          case 0x41:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* ASR rd, rs */
                   thumb_shift(alu_op, asr, reg);
                   break;

                case 0x01:
                   /* ADC rd, rs */
                   thumb_add(alu_op, rd, reg[rd], reg[rs], c_flag);
                   break;

                case 0x02:
                   /* SBC rd, rs */
                   thumb_sub(alu_op, rd, reg[rd], reg[rs], c_flag);
                   break;

                case 0x03:
                   /* ROR rd, rs */
                   thumb_shift(alu_op, ror, reg);
                   break;
             }
             break;

          case 0x42:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* TST rd, rs */
                   thumb_test_logic(alu_op, reg[rd] & reg[rs]);
                   break;

                case 0x01:
                   /* NEG rd, rs */
                   thumb_sub(alu_op, rd, 0, reg[rs], 1);
                   break;

                case 0x02:
                   /* CMP rd, rs */
                   thumb_test_sub(alu_op, reg[rd], reg[rs]);
                   break;

                case 0x03:
                   /* CMN rd, rs */
                   thumb_test_add(alu_op, reg[rd], reg[rs]);
                   break;
             }
             break;

          case 0x43:
             switch((opcode >> 6) & 0x03)
             {
                case 0x00:
                   /* ORR rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] | reg[rs]);
                   break;

                case 0x01:
                   /* MUL rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] * reg[rs]);
                   break;

                case 0x02:
                   /* BIC rd, rs */
                   thumb_logic(alu_op, rd, reg[rd] & (~reg[rs]));
                   break;

                case 0x03:
                   /* MVN rd, rs */
                   thumb_logic(alu_op, rd, ~reg[rs]);
                   break;
             }
             break;

          case 0x44:
             /* ADD rd, rs */
             thumb_hireg_op(reg[rd] + reg[rs]);
             break;

          case 0x45:
             /* CMP rd, rs */
             {
                thumb_pc_offset(4);
                thumb_decode_hireg_op();
                u32 _sa = reg[rd];
                u32 _sb = reg[rs];
                u32 dest = _sa - _sb;
                thumb_pc_offset(-2);
                calculate_flags_sub(dest, _sa, _sb, 1);
             }
             break;

          case 0x46:
             /* MOV rd, rs */
             thumb_hireg_op(reg[rs]);
             break;

          case 0x47:
             /* BX rs */
             {
                thumb_decode_hireg_op();
                u32 src;
                thumb_pc_offset(4);
                src = reg[rs];
                if(src & 0x01)
                {
                   reg[REG_PC] = src - 1;
                }
                else
                {
                   /* Switch to ARM mode */
                   reg[REG_PC] = src;
                   reg[REG_CPSR] &= ~0x20;
                   collapse_flags();
                   goto arm_loop;
                }
             }
             break;

          case 0x48 ... 0x4F:
             /* LDR r0..7, [pc + imm] */
             thumb_access_memory(load, imm, ((reg[REG_PC] - 2) & ~2) + (imm * 4) + 4, reg[(opcode >> 8) & 7], u32);
             break;

          case 0x50:
          case 0x51:
             /* STR rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u32);
             break;

          case 0x52:
          case 0x53:
             /* STRH rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u16);
             break;

          case 0x54:
          case 0x55:
             /* STRB rd, [rb + ro] */
             thumb_access_memory(store, mem_reg, reg[rb] + reg[ro], reg[rd], u8);
             break;

          case 0x56:
          case 0x57:
             /* LDSB rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], s8);
             break;

          case 0x58:
          case 0x59:
             /* LDR rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u32);
             break;

          case 0x5A:
          case 0x5B:
             /* LDRH rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u16);
             break;

          case 0x5C:
          case 0x5D:
             /* LDRB rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], u8);
             break;

          case 0x5E:
          case 0x5F:
             /* LDSH rd, [rb + ro] */
             thumb_access_memory(load, mem_reg, reg[rb] + reg[ro], reg[rd], s16);
             break;

          case 0x60 ... 0x67:
             /* STR rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + (imm * 4), reg[rd], u32);
             break;

          case 0x68 ... 0x6F:
             /* LDR rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + (imm * 4), reg[rd], u32);
             break;

          case 0x70 ... 0x77:
             /* STRB rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + imm, reg[rd], u8);
             break;

          case 0x78 ... 0x7F:
             /* LDRB rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + imm, reg[rd], u8);
             break;

          case 0x80 ... 0x87:
             /* STRH rd, [rb + imm] */
             thumb_access_memory(store, mem_imm, reg[rb] + (imm * 2), reg[rd], u16);
             break;

          case 0x88 ... 0x8F:
             /* LDRH rd, [rb + imm] */
             thumb_access_memory(load, mem_imm, reg[rb] + (imm * 2), reg[rd], u16);
             break;

          case 0x90 ... 0x97:
             /* STR r0..7, [sp + imm] */
             thumb_access_memory(store, imm, reg[REG_SP] + (imm * 4), reg[(opcode >> 8) & 7], u32);
             break;

          case 0x98 ... 0x9F:
             /* LDR r0..7, [sp + imm] */
             thumb_access_memory(load, imm, reg[REG_SP] + (imm * 4), reg[(opcode >> 8) & 7], u32);
             break;

          case 0xA0 ... 0xA7:
             /* ADD r0..7, pc, +imm */
             thumb_add_noflags(imm, ((opcode >> 8) & 7), (reg[REG_PC] & ~2) + 4, (imm * 4));
             break;

          case 0xA8 ... 0xAF:
             /* ADD r0..7, sp, +imm */
             thumb_add_noflags(imm, ((opcode >> 8) & 7), reg[REG_SP], (imm * 4));
             break;

          case 0xB0:
          case 0xB1:
          case 0xB2:
          case 0xB3:
             if((opcode >> 7) & 0x01)
             {
                /* ADD sp, -imm */
                thumb_add_noflags(add_sp, 13, reg[REG_SP], -(imm * 4));
             }
             else
             {
                /* ADD sp, +imm */
                thumb_add_noflags(add_sp, 13, reg[REG_SP], (imm * 4));
             }
             break;

          case 0xB4:  /* PUSH rlist */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPreDec>(
               REG_SP, opcode & 0xFF, cycles_remaining);
             break;

          case 0xB5:  /* PUSH rlist, lr */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPreDec>(
               REG_SP, (opcode & 0xFF) | (1 << REG_LR), cycles_remaining);
             break;

          case 0xBC:  /* POP rlist */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               REG_SP, opcode & 0xFF, cycles_remaining);
             break;

          case 0xBD:  /* POP rlist, pc */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               REG_SP, (opcode & 0xFF) | (1 << REG_PC), cycles_remaining);
             break;

          case 0xC0 ... 0xC7:    /* STMIA r0..7!, rlist */
             cpu_alert |= exec_thumb_block_mem<AccStore, AddrPostInc>(
               (opcode >> 8) & 7, (opcode & 0xFF), cycles_remaining);
             break;

          case 0xC8 ... 0xCF:    /* LDMIA r0..7!, rlist */
             cpu_alert |= exec_thumb_block_mem<AccLoad, AddrPostInc>(
               (opcode >> 8) & 7, (opcode & 0xFF), cycles_remaining);
             break;

          case 0xD0:   /* BEQ label */
             thumb_conditional_branch(z_flag == 1);
             break;
          case 0xD1:   /* BNE label */
             thumb_conditional_branch(z_flag == 0);
             break;
          case 0xD2:   /* BCS label */
             thumb_conditional_branch(c_flag == 1);
             break;
          case 0xD3:   /* BCC label */
             thumb_conditional_branch(c_flag == 0);
             break;
          case 0xD4:   /* BMI label */
             thumb_conditional_branch(n_flag == 1);
             break;
          case 0xD5:   /* BPL label */
             thumb_conditional_branch(n_flag == 0);
             break;
          case 0xD6:   /* BVS label */
             thumb_conditional_branch(v_flag == 1);
             break;
          case 0xD7:   /* BVC label */
             thumb_conditional_branch(v_flag == 0);
             break;
          case 0xD8:   /* BHI label */
             thumb_conditional_branch(c_flag & (z_flag ^ 1));
             break;
          case 0xD9:   /* BLS label */
             thumb_conditional_branch((c_flag == 0) | z_flag);
             break;
          case 0xDA:   /* BGE label */
             thumb_conditional_branch(n_flag == v_flag);
             break;
          case 0xDB:   /* BLT label */
             thumb_conditional_branch(n_flag != v_flag);
             break;
          case 0xDC:   /* BGT label */
             thumb_conditional_branch((z_flag == 0) & (n_flag == v_flag));
             break;
          case 0xDD:   /* BLE label */
             thumb_conditional_branch(z_flag | (n_flag != v_flag));
             break;

          case 0xDF:
#if SWI_HLE
             if (swi_hle((u32)(opcode & 0xFFu), &cpu_alert)) {
                reg[REG_BUS_VALUE] = 0xe3a02004; reg[REG_PC] += 2; break;   /* native → volgende instr */
             }
#endif
             collapse_flags();
             REG_MODE(MODE_SUPERVISOR)[6] = reg[REG_PC] + 2;
             REG_SPSR(MODE_SUPERVISOR) = reg[REG_CPSR];
             reg[REG_PC] = 0x00000008;
             // Move to ARM mode, Supervisor mode and disable IRQs
             reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
             set_cpu_mode(MODE_SUPERVISOR);
             reg[REG_BUS_VALUE] = 0xe3a02004;  // After SWI, we read bios[0xE4]
             goto arm_loop;
             break;

          case 0xE0 ... 0xE7:
             {
                /* B label */
                thumb_decode_branch();
                s32 br_offset = ((s32)(offset << 21) >> 20) + 4;
                reg[REG_PC] += br_offset;
                cycles_remaining -= ws_cyc_nseq[reg[REG_PC] >> 24][0];
                break;
             }

          case 0xF0 ... 0xF7:
             {
                /* (low word) BL label */
                thumb_decode_branch();
                reg[REG_LR] = reg[REG_PC] + 4 + ((s32)(offset << 21) >> 9);
                thumb_pc_offset(2);
                break;
             }

          case 0xF8 ... 0xFF:
             {
                /* (high word) BL label */
                thumb_decode_branch();
                u32 newlr = (reg[REG_PC] + 2) | 0x01;
                u32 newpc = reg[REG_LR] + (offset * 2);
                reg[REG_LR] = newlr;
                reg[REG_PC] = newpc;
                cycles_remaining -= ws_cyc_nseq[newpc >> 24][0];
                break;
             }
       }

       /* End of Execute THUMB instruction */
       cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][0];

       if (reg[REG_PC] == idle_loop_target_pc && cycles_remaining > 0) cycles_remaining = 0;

       if (cpu_alert & (CPU_ALERT_HALT | CPU_ALERT_IRQ))
          goto alert;

    } while(cycles_remaining > 0);

    collapse_flags();
    update_ret = update_gba(cycles_remaining);
    if (completed_frame(update_ret))
       return;
    cycles_remaining = cycles_to_run(update_ret);
    continue;

    alert:
      /* CPU stopped or switch to IRQ handler */
      collapse_flags();
  }
}

void init_cpu(void)
{
  // Initialize CPU registers
  memset(reg, 0, REG_USERDEF * sizeof(u32));
  memset(reg_mode, 0, sizeof(reg_mode));
  for (u32 i = 0; i < sizeof(spsr)/sizeof(spsr[0]); i++)
    spsr[i] = 0x00000010;

  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  reg[REG_SLEEP_CYCLES] = 0;

  if (selected_boot_mode == boot_game) {
    reg[REG_SP] = 0x03007F00;
    reg[REG_PC] = 0x08000000;
    reg[REG_CPSR] = 0x0000001F;   // system mode
    reg[CPU_MODE] = MODE_SYSTEM;
  } else {
    reg[REG_SP] = 0x03007F00;
    reg[REG_PC] = 0x00000000;
    reg[REG_CPSR] = 0x00000013 | 0xC0;  // supervisor
    reg[CPU_MODE] = MODE_SUPERVISOR;
  }

  // Stack pointers are set by BIOS, we set them
  // nevertheless, should we not boot from BIOS
  REG_MODE(MODE_USER)[5] = 0x03007F00;
  REG_MODE(MODE_IRQ)[5] = 0x03007FA0;
  REG_MODE(MODE_FIQ)[5] = 0x03007FA0;
  REG_MODE(MODE_SUPERVISOR)[5] = 0x03007FE0;
}

bool cpu_check_savestate(const u8 *src)
{
  const u8 *cpudoc = bson_find_key(src, "cpu");
  if (!cpudoc)
    return false;

  return bson_contains_key(cpudoc, "bus-value", BSON_TYPE_INT32) &&
         bson_contains_key(cpudoc, "regs", BSON_TYPE_ARR) &&
         bson_contains_key(cpudoc, "spsr", BSON_TYPE_ARR) &&
         bson_contains_key(cpudoc, "regmod", BSON_TYPE_ARR);
}


bool cpu_read_savestate(const u8 *src)
{
  const u8 *cpudoc = bson_find_key(src, "cpu");
  return bson_read_int32(cpudoc, "bus-value", &reg[REG_BUS_VALUE]) &&
         bson_read_int32_array(cpudoc, "regs", reg, REG_ARCH_COUNT) &&
         bson_read_int32_array(cpudoc, "spsr", spsr, 6) &&
         bson_read_int32_array(cpudoc, "regmod", (u32*)reg_mode, 7*7);
}

unsigned cpu_write_savestate(u8 *dst)
{
  u8 *wbptr, *startp = dst;
  bson_start_document(dst, "cpu", wbptr);
  bson_write_int32array(dst, "regs", reg, REG_ARCH_COUNT);
  bson_write_int32array(dst, "spsr", spsr, 6);
  bson_write_int32array(dst, "regmod", reg_mode, 7*7);
  bson_write_int32(dst, "bus-value", reg[REG_BUS_VALUE]);

  bson_finish_document(dst, wbptr);
  return (unsigned int)(dst - startp);
}


