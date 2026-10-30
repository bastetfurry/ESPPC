// =============================================================================
//
// Based on code from:
//    * 8086tiny: a tiny, highly functional, highly portable PC emulator/VM
//      Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
//    * 8086tiny plus Revision 1.34 - Copyright 2014 Julian Olds - https://jaybertsoftware.weebly.com/8086-tiny-plus.html
//
// This work is licensed under the MIT License. See included LICENSE.TXT.
//
// Changes by Fabrizio Di Vittorio
//   - some heavy optimizations
//   - bug fixes on several instructions (HLT, divide by zero interrupt, ROL, ROR, RCL, RCR, SHL, SHR, DAA, DAS)
//   - expanded macros
//   - removed redundant code resulted from macro expansions
//   - moved Flags out of registers file
//   - moved some static variables into auto function vars
//   - moved decodes tables from BIOS to C code
//   - emulator commands executed as INT instead of custom CPU opcodes
//   - reset to 0xffff:0000 as real 8086
//   - LEA, removed mod=11 option
//   - registers moved to different area
//   - memory read/write no more direct but by callbacks



#include <stdio.h>
#include <string.h>

#include "i8086.h"


namespace fabgl {



#pragma GCC optimize ("O3")

#if FABGL_ESP_IDF_VERSION > FABGL_ESP_IDF_VERSION_VAL(3, 3, 5)
  #pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif


// ESPPC: the start of the video window is runtime-selectable, see i8086::setVideoMemStart().
// The machine's video memory only covers 0xB0000-0xBFFFF (Machine::writeVideoMemory8 discards
// anything below that), so trapping from 0xA0000 leaves a 64KB hole that reads 0xFF and swallows
// writes. A machine configured with more than 640KB moves the window to 0xB0000, turning that
// hole into ordinary RAM. Machines at 640KB or less keep 0xA0000 for IBM-compatible behaviour.
#define VIDEOMEM_END   0xC0000


// 16-bit register decodes

#define REG_AX                                   0
#define REG_CX                                   1
#define REG_DX                                   2
#define REG_BX                                   3
#define REG_SP                                   4
#define REG_BP                                   5
#define REG_SI                                   6
#define REG_DI                                   7

#define REG_ES                                   8
#define REG_CS                                   9
#define REG_SS                                   10
#define REG_DS                                   11

#define REG_ZERO                                 12
#define REG_SCRATCH                              13

#define REG_TMP                                  15


// 8-bit register decodes
#define REG_AL                                   0
#define REG_AH                                   1
#define REG_CL                                   2
#define REG_CH                                   3
#define REG_DL                                   4
#define REG_DH                                   5
#define REG_BL                                   6
#define REG_BH                                   7


// FLAGS

#define CF_ADDR 0
#define PF_ADDR 1
#define AF_ADDR 2
#define ZF_ADDR 3
#define SF_ADDR 4
#define TF_ADDR 5
#define IF_ADDR 6
#define DF_ADDR 7
#define OF_ADDR 8
#define XX_ADDR 9

#define FLAG_CF                                  (flags[CF_ADDR])
#define FLAG_PF                                  (flags[PF_ADDR])
#define FLAG_AF                                  (flags[AF_ADDR])
#define FLAG_ZF                                  (flags[ZF_ADDR])
#define FLAG_SF                                  (flags[SF_ADDR])
#define FLAG_TF                                  (flags[TF_ADDR])
#define FLAG_IF                                  (flags[IF_ADDR])
#define FLAG_DF                                  (flags[DF_ADDR])
#define FLAG_OF                                  (flags[OF_ADDR])



// Global variable definitions
// Everything the core works on per instruction is in i8086::s_state (see i8086.h); the code below
// keeps its original names for it.
// Having the registers and flags in arrays is actually faster on the target platform, the ESP32 with its Xtensa cores.
i8086::State i8086::s_state;
#define regs            i8086::s_state.regs
#define flags           i8086::s_state.flags
#define reg_ip          i8086::s_state.regIP
#define regs8           regs
#define regs16          ((uint16_t *) regs)
#define regs_offset     i8086::s_state.regs_offset
#define op_source       i8086::s_state.op_source
#define op_dest         i8086::s_state.op_dest
#define set_flags_type  i8086::s_state.set_flags_type
#define op_to_addr      i8086::s_state.op_to_addr
#define op_from_addr    i8086::s_state.op_from_addr
#define seg_override    i8086::s_state.seg_override
#define i_mod_size      i8086::s_state.i_mod_size
#define i_d             i8086::s_state.i_d
#define i_w             i8086::s_state.i_w
#define raw_opcode_id   i8086::s_state.raw_opcode_id
#define xlat_opcode_id  i8086::s_state.xlat_opcode_id
#define extra           i8086::s_state.extra
#define rep_mode        i8086::s_state.rep_mode
#define seg_override_en i8086::s_state.seg_override_en
#define rep_override_en i8086::s_state.rep_override_en
#define optcodes        i8086::s_state.optcodes
#define parity          i8086::s_state.parity
#define ex_data         i8086::s_state.ex_data
#define xlat_ids        i8086::s_state.xlat_ids
#define std_flags       i8086::s_state.std_flags
#define i_mod_adder     i8086::s_state.i_mod_adder
#define base_size       i8086::s_state.base_size
#define i_w_adder       i8086::s_state.i_w_adder
#define jxx_dec_a       i8086::s_state.jxx_dec_a
#define jxx_dec_b       i8086::s_state.jxx_dec_b
#define jxx_dec_c       i8086::s_state.jxx_dec_c
#define jxx_dec_d       i8086::s_state.jxx_dec_d
#define RM_BASE         i8086::s_state.RM_BASE
#define RM_INDEX        i8086::s_state.RM_INDEX




// Table 0: R/M mode 1/2 "register 1" lookup
// ESPPC: every table the core reads is in internal RAM. Static arrays that are never written are
// moved to .rodata by the compiler, which is flash, read through the cache the guest's RAM in PSRAM
// keeps evicting: optcodes (read at every instruction), the ModRM and the Jcc tables had gone there.
// The ones below are DRAM_ATTR; those named ..._init are only templates, read once by initTables()
// into i8086::s_state, where the core finds them next to its other state.
static uint8_t DRAM_ATTR rm_mode12_reg1[]  = { 3, 3, 5, 5, 6, 7, 5, 3 };

// Table 1: R/M mode 1/2 "register 2" lookup
// Table 5: R/M mode 0 "register 2" lookup
static uint8_t DRAM_ATTR rm_mode012_reg2[] = { 6, 7, 6, 7, 12, 12, 12, 12 };

// Table 2: R/M mode 1/2 "DISP multiplier" lookup
static uint8_t DRAM_ATTR rm_mode12_disp[]  = { 1, 1, 1, 1, 1, 1, 1, 1 };

// Table 3: R/M mode 1/2 "default segment" lookup
static uint8_t DRAM_ATTR rm_mode12_dfseg[] = { 11, 11, 10, 10, 11, 11, 10, 11 };

// Table 4: R/M mode 0 "register 1" lookup
static uint8_t DRAM_ATTR rm_mode0_reg1[]   = { 3, 3, 5, 5, 6, 7, 12, 3 };

// Table 6: R/M mode 0 "DISP multiplier" lookup
static uint8_t DRAM_ATTR rm_mode0_disp[]   = { 0, 0, 0, 0, 0, 0, 1, 0 };

// Table 7: R/M mode 0 "default segment" lookup
static uint8_t DRAM_ATTR rm_mode0_dfseg[]  = { 11, 11, 10, 10, 11, 11, 11, 11 };

// Table 8: Translation of raw opcode index ("Raw ID") to function number ("Xlat'd ID")
static const uint8_t xlat_ids_init[256] = {   9,  9,  9,  9,  7,  7, 25, 26,  9,  9,  9,  9,  7,  7, 25, 50,
                                9,  9,  9,  9,  7,  7, 25, 26,  9,  9,  9,  9,  7,  7, 25, 26,
                                9,  9,  9,  9,  7,  7, 27, 28,  9,  9,  9,  9,  7,  7, 27, 28,
                                9,  9,  9,  9,  7,  7, 27, 29,  9,  9,  9,  9,  7,  7, 27, 29,
                                2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
                                3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,
                                53, 54, 55, 70, 71, 71, 72, 72, 56, 58, 57, 58, 59, 59, 60, 60,
                                0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
                                8,  8,  8,  8, 15, 15, 24, 24,  9,  9,  9,  9, 10, 10, 10, 10,
                                16, 16, 16, 16, 16, 16, 16, 16, 30, 31, 32, 69, 33, 34, 35, 36,
                                11, 11, 11, 11, 17, 17, 18, 18, 47, 47, 17, 17, 17, 17, 18, 18,
                                1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
                                12, 12, 19, 19, 37, 37, 20, 20, 51, 52, 19, 19, 38, 39, 40, 19,
                                12, 12, 12, 12, 41, 42, 43, 44, 69, 69, 69, 69, 69, 69, 69, 69,
                                13, 13, 13, 13, 21, 21, 22, 22, 14, 14, 14, 14, 21, 21, 22, 22,
                                48,  0, 23, 23, 49, 45,  6,  6, 46, 46, 46, 46, 46, 46,  5,  5 };

// Table 9: Translation of Raw ID to Extra Data
static const uint8_t ex_data_init[256] = {  0,  0,  0,  0,  0,  0,  8,  8,  1,  1,  1,  1,  1,  1,  9, 36,
                              2,  2,  2,  2,  2,  2, 10, 10,  3,  3,  3,  3,  3,  3, 11, 11,
                              4,  4,  4,  4,  4,  4,  8,  0,  5,  5,  5,  5,  5,  5,  9,  1,
                              6,  6,  6,  6,  6,  6, 10,  2,  7,  7,  7,  7,  7,  7, 11,  0,
                              0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,
                              0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
                              0,  0, 21, 21, 21, 21, 21, 21,  0,  0,  0,  0, 21, 21, 21, 21,
                              21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
                              0,  0,  0,  0,  0,  0,  0,  0,  8,  8,  8,  8, 12, 12, 12, 12,
                              0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 255, 0,
                              0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  2,  2,  1,  1,
                              0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
                              1,  1,  0,  0, 16, 22,  0,  0,  0,  0,  1,  1,  0, 255, 48, 2,
                              0,  0,  0,  0, 255, 255, 40, 11, 3, 3,  3,  3,  3,  3,  3,  3,
                              43, 43, 43, 43,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
                              1, 21,  0,  0,  2, 40, 21, 21, 80, 81, 92, 93, 94, 95,  0,  0 };

// Table 10: How each Raw ID sets the flags (bit 1 = sets SZP, bit 2 = sets AF/OF for arithmetic, bit 3 = sets OF/CF for logic)
static const uint8_t std_flags_init[256] = {  3, 3, 3, 3, 3, 3, 0, 0, 5, 5, 5, 5, 5, 5, 0, 0,
                                1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0,
                                5, 5, 5, 5, 5, 5, 0, 1, 3, 3, 3, 3, 3, 3, 0, 1,
                                5, 5, 5, 5, 5, 5, 0, 1, 3, 3, 3, 3, 3, 3, 0, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                1, 1, 1, 1, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Table 11: Parity flag loop-up table (256 entries)
static const uint8_t parity_init[256] = {   1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                              1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1 };

// Table 12: Translation of Raw ID to base instruction size (bytes)
static const uint8_t base_size_init[256] = {  2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 2,
                                2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1,
                                2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1,
                                2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 2, 1, 1, 1, 1, 1,
                                2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,
                                3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                3, 3, 0, 0, 2, 2, 2, 2, 4, 1, 0, 0, 0, 0, 0, 0,
                                2, 2, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
                                2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 1,
                                1, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2 };

// Table 13: Translation of Raw ID to i_w size adder yes/no
static const uint8_t i_w_adder_init[256] = {  0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0,
                                0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0,
                                0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0,
                                0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Table 14: Translation of Raw ID to i_mod size adder yes/no
static const uint8_t i_mod_adder_init[256] = {  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                  1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1 };

// Table 15: Jxx decode table A
static const uint8_t jxx_dec_a_init[8] = { OF_ADDR, CF_ADDR, ZF_ADDR, CF_ADDR, SF_ADDR, PF_ADDR, XX_ADDR, XX_ADDR };

// Table 16: Jxx decode table B
static const uint8_t jxx_dec_b_init[8] = { XX_ADDR, XX_ADDR, XX_ADDR, ZF_ADDR, XX_ADDR, XX_ADDR, XX_ADDR, ZF_ADDR };

// Table 17: Jxx decode table C
static const uint8_t jxx_dec_c_init[8] = { XX_ADDR, XX_ADDR, XX_ADDR,XX_ADDR, XX_ADDR, XX_ADDR, SF_ADDR, SF_ADDR };

// Table 18: Jxx decode table D
static const uint8_t jxx_dec_d_init[8] = { XX_ADDR, XX_ADDR, XX_ADDR, XX_ADDR,XX_ADDR, XX_ADDR, OF_ADDR, OF_ADDR };


DRAM_ATTR static uint8_t * instr_table_lookup[] = {    rm_mode12_reg1,    // 0
                                             rm_mode012_reg2,   // 1
                                             rm_mode12_disp,    // 2
                                             rm_mode12_dfseg,   // 3
                                             rm_mode0_reg1,     // 4
                                             rm_mode012_reg2,   // 5
                                             rm_mode0_disp,     // 6
                                             rm_mode0_dfseg,    // 7
                                             xlat_ids,          // 8 TABLE_XLAT_OPCODE
                                             ex_data,           // 9 TABLE_XLAT_SUBFUNCTION
                                             std_flags,         // 10 TABLE_STD_FLAGS
                                             parity,            // 11 TABLE_PARITY_FLAG
                                             base_size,         // 12 TABLE_BASE_INST_SIZE
                                             i_w_adder,         // 13 TABLE_I_W_SIZE
                                             i_mod_adder,       // 14 TABLE_I_MOD_SIZE
                                          };




// ret false if not acked
// ESPPC: in IRAM, the machine's loop calls it whenever an interrupt is pending
bool IRAM_ATTR i8086::IRQ(uint8_t interrupt_num)
{
  if (!s_state.pendingIRQ) {
    s_state.pendingIRQ = true;
    s_state.pendingIRQIndex = interrupt_num;
    return true;
  } else {
    return false;
  }
}



/////////////////////////////////////////////////////////////////////////////


// direct RAM access (not video RAM)
#define MEM8(addr)  s_state.memory[addr]
#define MEM16(addr) (*(uint16_t*)(s_state.memory + (addr)))


inline __attribute__((always_inline)) uint8_t i8086::RMEM8(int addr)
{
  if (addr >= s_state.videoMemStart && addr < VIDEOMEM_END) {
    return s_state.readVideoMemory8(s_state.context, addr);
  } else {
    return s_state.memory[addr];
  }
}


inline __attribute__((always_inline)) uint16_t i8086::RMEM16(int addr)
{
  if (addr >= s_state.videoMemStart && addr < VIDEOMEM_END) {
    return s_state.readVideoMemory16(s_state.context, addr);
  } else {
    return *(uint16_t*)(s_state.memory + addr);
  }
}


inline __attribute__((always_inline)) uint8_t i8086::WMEM8(int addr, uint8_t value)
{
  if (addr >= s_state.videoMemStart && addr < VIDEOMEM_END) {
    s_state.writeVideoMemory8(s_state.context, addr, value);
  } else {
    s_state.memory[addr] = value;
  }
  return value;
}


inline __attribute__((always_inline)) uint16_t i8086::WMEM16(int addr, uint16_t value)
{
  if (addr >= s_state.videoMemStart && addr < VIDEOMEM_END) {
    s_state.writeVideoMemory16(s_state.context, addr, value);
  } else {
    *(uint16_t*)(s_state.memory + addr) = value;
  }
  return value;
}

/////////////////////////////////////////////////////////////////////////////



// Set auxiliary and overflow flag after arithmetic operations
static inline __attribute__((always_inline)) void set_AF_OF_arith(int32_t op_result)
{
  FLAG_AF = (bool)((op_source ^= op_dest ^ op_result) & 0x10);
  if (op_result == op_dest)
    FLAG_OF = 0;
  else
    FLAG_OF = 1 & (FLAG_CF ^ op_source >> (8 * (i_w + 1) - 1));
}


// Assemble and return emulated CPU FLAGS register
uint16_t i8086::make_flags()
{
  #if I80186MODE
  uint16_t r = 0x0002;    // to pass test186 tests, just unused bit nr. 1 is set to 1 (some programs checks this to know if this is a 80186 or 8086)
  #else
  uint16_t r = 0xf002;    // for real 8086
  #endif

  return r | FLAG_CF << 0 | FLAG_PF << 2 | FLAG_AF << 4 | FLAG_ZF << 6 | FLAG_SF << 7 | FLAG_TF << 8 | FLAG_IF << 9 | FLAG_DF << 10 | FLAG_OF << 11;
}


void i8086::set_flags(int new_flags)
{
  FLAG_CF = (bool)(new_flags & 0x001);
  FLAG_PF = (bool)(new_flags & 0x004);
  FLAG_AF = (bool)(new_flags & 0x010);
  FLAG_ZF = (bool)(new_flags & 0x040);
  FLAG_SF = (bool)(new_flags & 0x080);
  FLAG_TF = (bool)(new_flags & 0x100);
  FLAG_IF = (bool)(new_flags & 0x200);
  FLAG_DF = (bool)(new_flags & 0x400);
  FLAG_OF = (bool)(new_flags & 0x800);
}


// Convert raw opcode to translated opcode index. This condenses a large number of different encodings of similar
// instructions into a much smaller number of distinct functions, which we then execute
static inline __attribute__((always_inline)) void set_opcode(uint8_t opcode)
{
  raw_opcode_id  = opcode;
  xlat_opcode_id = xlat_ids[opcode];
  extra          = ex_data[opcode];
  i_mod_size     = i_mod_adder[opcode];
  set_flags_type = std_flags[opcode];
}


// Execute INT #interrupt_num on the emulated machine
// ESPPC: in IRAM, taken on every software INT and hardware IRQ
void IRAM_ATTR i8086::pc_interrupt(uint8_t interrupt_num)
{
  // fab: interrupt can exit from halt state
  if (s_state.halted) {
    s_state.halted = false;
    ++reg_ip; // go to next instruction after HLT
  }

  if (!s_state.interrupt(s_state.context, interrupt_num)) {
    uint16_t newIP     = MEM16(4 * interrupt_num);
    uint16_t newCS     = MEM16(4 * interrupt_num + 2);

    regs16[REG_SP] -= 6;
    uint16_t * stack = &MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
    stack[2] = make_flags();
    stack[1] = regs16[REG_CS];
    stack[0] = reg_ip;

    reg_ip         = newIP;
    regs16[REG_CS] = newCS;

    FLAG_TF = FLAG_IF = 0;
  }
}


// fab: necessary to go back to the first instruction prefix
uint8_t i8086::raiseDivideByZeroInterrupt()
{
  if (seg_override_en || rep_override_en) {
    // go back looking for segment prefixes or REP prefixes
    while (true) {
      uint8_t opcode = MEM8(16 * regs16[REG_CS] + reg_ip - 1);
      // break if not REP and SEG
      if ((opcode & 0xfe) != 0xf2 && (opcode & 0xe7) != 0x26)
        break;
      --reg_ip;
    }
  }
  pc_interrupt(0);
  return 0;
}


// AAA and AAS instructions - which_operation is +1 for AAA, and -1 for AAS
static int32_t AAA_AAS()
{
  FLAG_AF = FLAG_CF = ((regs8[REG_AL] & 0x0F) > 9) || FLAG_AF;
  regs16[REG_AX] += 262 * (extra - 1) * FLAG_AF;
  return regs8[REG_AL] &= 0x0F;
}


static int32_t DAA_DAS()
{
  // fab: fixed to pass test186
  i_w = 0;
  FLAG_AF = (regs8[REG_AL] & 0x0f) > 9 || FLAG_AF;
  FLAG_CF = regs8[REG_AL] > 0x99 || FLAG_CF;
  if (extra) {
    // DAS
    if (FLAG_CF)
      regs8[REG_AL] -= 0x60;
    else if (FLAG_AF)
      FLAG_CF = (regs8[REG_AL] < 6);
    if (FLAG_AF)
      regs8[REG_AL] -= 6;
  } else {
    // DAA
    if (FLAG_CF)
      regs8[REG_AL] += 0x60;
    if (FLAG_AF)
      regs8[REG_AL] += 6;
  }
  return regs8[REG_AL];
}


static void initTables();


void i8086::reset()
{
  initTables();

  regs_offset = (int32_t)(regs - s_state.memory);

  memset(regs8, 0, sizeof(regs));
  set_flags(0);

  // Initialise CPU state variables
  seg_override_en = 0;
  rep_override_en = 0;

  s_state.halted = false;
  s_state.trap   = false;
  s_state.intr   = false;   // ESPPC: the INTR line comes back from the PIC with the next batch

  regs16[REG_CS] = 0xffff;
  reg_ip = 0;
}


// tries to make more compact opcode "switch"
static const uint8_t optcodes_init[256] = {
// 0    1   2   3   4   5   6   7   8   9   a   b   c   d   e   f
  27, 27, 27, 27, 28, 28, 13, 12, 27, 27, 27, 27, 28, 28, 13,  0, // 00 - 0f
  27, 27, 27, 27, 28, 28, 13, 12, 27, 27, 27, 27, 28, 28, 13, 12, // 10 - 1f
  27, 27, 27, 27, 28, 28,  1,  0, 27, 27, 27, 27, 28, 28,  1,  0, // 20 - 2f
  27, 27, 27, 27, 28, 28,  1,  0, 27, 27, 27, 27, 28, 28,  1,  0, // 30 - 3f
  19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, // 40 - 4f
   9,  9,  9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  8,  8, // 50 - 5f
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // 60 - 6f
   2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, // 70 - 7f
  22, 22, 22, 22, 34, 34,  0,  0, 21, 21, 21, 21, 37,  0, 18, 35, // 80 - 8f
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 32,  0,  0,  0,  0,  0, // 90 - 9f
  31, 31, 31, 31, 26, 26,  0,  0, 28, 28, 26, 26, 26, 26,  0,  0, // a0 - af
  10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, // b0 - bf
   0,  0, 36,  7,  0,  0, 25, 25,  0,  0, 36, 17,  0, 15,  0, 16, // c0 - cf
  23, 23, 23, 23,  0,  0,  0, 38,  0,  0,  0,  0,  0,  0,  0,  0, // d0 - df
   0,  0,  0,  5, 20, 20, 20, 20,  6, 33,  0,  3, 20, 20, 20, 20, // e0 - ef
   0,  0, 14, 14,  0,  0, 29, 29,  4,  4,  4,  4,  4,  4, 24, 30, // f0 - ff
};


// ESPPC: helpers for the fast paths in step().
//
// A decoded ModRM operand: a register (idx into regs8 or regs16) or a memory address, and the
// instruction bytes it took from the ModRM byte on (1 to 3). Same effective addresses as stepEx:
// base + index + displacement, SS the default segment when BP is the base, and a segment override
// prefix taking over.
struct FastRM {
  bool     reg;
  uint8_t  idx;
  uint8_t  len;
  int32_t  addr;
};

static const uint8_t RM_BASE_init[8] = { REG_BX, REG_BX, REG_BP, REG_BP, REG_SI, REG_DI, REG_BP, REG_BX };
static const uint8_t RM_INDEX_init[8] = { REG_SI, REG_DI, REG_SI, REG_DI, REG_ZERO, REG_ZERO, REG_ZERO, REG_ZERO };

// the tables into s_state, next to the state they work on (see i8086.h); reset() calls it
static void initTables()
{
  memcpy(optcodes, optcodes_init, sizeof(optcodes));
  memcpy(parity, parity_init, sizeof(parity));
  memcpy(ex_data, ex_data_init, sizeof(ex_data));
  memcpy(xlat_ids, xlat_ids_init, sizeof(xlat_ids));
  memcpy(std_flags, std_flags_init, sizeof(std_flags));
  memcpy(i_mod_adder, i_mod_adder_init, sizeof(i_mod_adder));
  memcpy(base_size, base_size_init, sizeof(base_size));
  memcpy(i_w_adder, i_w_adder_init, sizeof(i_w_adder));
  memcpy(jxx_dec_a, jxx_dec_a_init, sizeof(jxx_dec_a));
  memcpy(jxx_dec_b, jxx_dec_b_init, sizeof(jxx_dec_b));
  memcpy(jxx_dec_c, jxx_dec_c_init, sizeof(jxx_dec_c));
  memcpy(jxx_dec_d, jxx_dec_d_init, sizeof(jxx_dec_d));
  memcpy(RM_BASE, RM_BASE_init, sizeof(RM_BASE));
  memcpy(RM_INDEX, RM_INDEX_init, sizeof(RM_INDEX));
}

static inline __attribute__((always_inline)) FastRM decodeRM(uint8_t const * p, bool w)
{
  FastRM  r;
  uint8_t m   = p[0];
  uint8_t mod = m >> 6;
  uint8_t rm  = m & 7;
  if (mod == 3) {
    r.reg  = true;
    r.idx  = w ? rm : ((2 * rm + rm / 4) & 7);      // AL CL DL BL AH CH DH BH
    r.len  = 1;
    r.addr = 0;
    return r;
  }
  uint16_t off;
  bool     bp;
  if (mod == 0 && rm == 6) {
    off   = p[1] | (p[2] << 8);
    r.len = 3;
    bp    = false;
  } else {
    off = regs16[RM_BASE[rm]] + regs16[RM_INDEX[rm]];
    if (mod == 1) {
      off  += (int8_t) p[1];
      r.len = 2;
    } else if (mod == 2) {
      off  += p[1] | (p[2] << 8);
      r.len = 3;
    } else
      r.len = 1;
    bp = (rm == 2 || rm == 3 || rm == 6);
  }
  r.reg  = false;
  r.idx  = 0;
  r.addr = 16 * regs16[seg_override_en ? seg_override : (bp ? REG_SS : REG_DS)] + off;
  return r;
}

// ADD OR ADC SBB AND SUB XOR CMP on 8 or 16 bits, with the flags the 8086 sets (AF is left alone by
// the logical ones, where it is undefined)
static inline __attribute__((always_inline)) uint32_t fastALU(int op, uint32_t a, uint32_t b, bool w)
{
  uint32_t mask = w ? 0xffff : 0xff;
  uint32_t sign = w ? 0x8000 : 0x80;
  uint32_t res, c;
  switch (op) {
    case 0:  // ADD
    case 2:  // ADC
      c   = (op == 2) ? FLAG_CF : 0;
      res = a + b + c;
      FLAG_CF = res > mask;
      FLAG_OF = !!(~(a ^ b) & (a ^ res) & sign);
      FLAG_AF = !!((a ^ b ^ res) & 0x10);
      break;
    case 3:  // SBB
    case 5:  // SUB
    case 7:  // CMP
      c   = (op == 3) ? FLAG_CF : 0;
      res = a - b - c;
      FLAG_CF = a < b + c;
      FLAG_OF = !!((a ^ b) & (a ^ res) & sign);
      FLAG_AF = !!((a ^ b ^ res) & 0x10);
      break;
    case 1:  res = a | b; FLAG_CF = FLAG_OF = 0; break;   // OR
    case 4:  res = a & b; FLAG_CF = FLAG_OF = 0; break;   // AND
    default: res = a ^ b; FLAG_CF = FLAG_OF = 0; break;   // XOR
  }
  res &= mask;
  FLAG_SF = !!(res & sign);
  FLAG_ZF = !res;
  FLAG_PF = parity[(uint8_t) res];
  return res;
}

// ROL ROR RCL RCR SHL SHR SAR by one (op 6, the undocumented one, stays with stepEx). Rotates touch
// CF and OF only; shifts set SF, ZF and PF too.
static inline __attribute__((always_inline)) uint32_t fastShift1(int op, uint32_t v, bool w)
{
  uint32_t top = w ? 15 : 7;
  uint32_t msk = w ? 0xffff : 0xff;
  uint32_t res;
  switch (op) {
    case 0:  // ROL
      FLAG_CF = (v >> top) & 1;
      res     = ((v << 1) | FLAG_CF) & msk;
      FLAG_OF = ((res >> top) & 1) ^ FLAG_CF;
      return res;
    case 1:  // ROR
      FLAG_CF = v & 1;
      res     = (v >> 1) | (FLAG_CF << top);
      FLAG_OF = ((res >> top) ^ (res >> (top - 1))) & 1;
      return res;
    case 2:  // RCL
    {
      uint32_t c = FLAG_CF;
      FLAG_CF = (v >> top) & 1;
      res     = ((v << 1) | c) & msk;
      FLAG_OF = ((res >> top) & 1) ^ FLAG_CF;
      return res;
    }
    case 3:  // RCR
    {
      uint32_t c = FLAG_CF;
      FLAG_CF = v & 1;
      res     = (v >> 1) | (c << top);
      FLAG_OF = ((res >> top) ^ (res >> (top - 1))) & 1;
      return res;
    }
    case 4:  // SHL
      FLAG_CF = (v >> top) & 1;
      res     = (v << 1) & msk;
      FLAG_OF = ((res >> top) & 1) ^ FLAG_CF;
      break;
    case 5:  // SHR
      FLAG_CF = v & 1;
      res     = v >> 1;
      FLAG_OF = (v >> top) & 1;
      break;
    default: // SAR
      FLAG_CF = v & 1;
      res     = (v >> 1) | (v & (1u << top));
      FLAG_OF = 0;
      break;
  }
  FLAG_SF = (res >> top) & 1;
  FLAG_ZF = !res;
  FLAG_PF = parity[(uint8_t) res];
  return res;
}


// ESPPC: in IRAM. It runs every emulated instruction, and from flash it competes for the cache
// with guest RAM in PSRAM, which both go through. 'count' instructions per call: runTask runs a
// whole batch in one, which saves the call (call8, entry and retw, about 14 cycles, the monitor's
// 'n') for all but the first, and lets the state's base stay in a register. Every 'return' of an
// instruction is a 'goto next'.
void IRAM_ATTR i8086::step(int count)
{
begin:

  // Interrupts handling
  // Following instructions inhibits interrupts for the next instruction:
  //    MOV SS, xxx     [opcode 0x8e]
  //    POP SS          [opcode 0x17]
  //    STI             [opcode 0xfb]
  // They are hard coded inside below loop, repeating it using "continue" instead of "goto next"

  // Application has set trap flag, so fire INT 1
  // ESPPC: after an instruction that began with TF set, as the 8086 does: the one after the IRET or
  // POPF that sets TF runs before the first trap, and the one that clears TF still traps. Taken
  // before it, a handler that IRETs back with TF set got the trap again at once and never returned
  // to the code it steps (CheckIt's "CPU Interrupt Bug" test hung there).
  if (s_state.trap && !seg_override_en && !rep_override_en) {
    pc_interrupt(1);
  }
  // Check for interrupts triggered by system interfaces
  else if (FLAG_IF && (s_state.pendingIRQ || s_state.intr) && !seg_override_en && !rep_override_en) {
    if (s_state.pendingIRQ) {
      pc_interrupt(s_state.pendingIRQIndex);
      s_state.pendingIRQ = false;
    } else {
      // ESPPC: INTR, acknowledged now that the CPU takes it (see setINTR())
      s_state.intr = false;
      int vector = s_state.acknowledge(s_state.context);
      if (vector >= 0)
        pc_interrupt(vector);
    }
  }

  s_state.trap = FLAG_TF;

  // Instructions handling

  PSRAM_WORKAROUND2
  
  // external interrupts allowed only out of this loop
  while (true) {

    // seg_override_en and rep_override_en contain number of instructions to hold segment override and REP prefix respectively
    if (seg_override_en)
      --seg_override_en;
    if (rep_override_en)
      --rep_override_en;

    uint8_t const * opcode_stream = s_state.memory + 16 * regs16[REG_CS] + reg_ip;

    #if I8086_SHOW_OPCODE_STATS
    static uint32_t opcodeStats[256] = {0};
    static int      opcodeStatsCount = 0;
    static uint64_t opcodeStatsT0 = esp_timer_get_time();
    opcodeStats[*opcode_stream] += 1;
    ++opcodeStatsCount;
    if ((opcodeStatsCount % 500000000) == 0) {
      opcodeStatsCount = 0;
      if (Serial.available()) {
        Serial.read();
        printf("\ntime delta = %llu uS\n\n", esp_timer_get_time() - opcodeStatsT0);
        opcodeStatsT0 = esp_timer_get_time();
        for (int i = 0; i < 256; ++i) {
          if (opcodeStats[i] > 0)
            printf("%d, %02X\n", opcodeStats[i], i);
          opcodeStats[i] = 0;
        }
        printf("\n");
      }
    }
    #endif

    // quick and dirty processing of simple and common instructions
    // ESPPC: dispatch through a table of label addresses in DRAM. The IDF builds with
    // -fno-jump-tables (a switch's table would be in flash), so the switch that was here became
    // a tree of compares and branches: 13 instructions on average for the opcodes Doom runs
    // most, 8 of them taken branches. Index 0 is everything left to stepEx. 'continue' is the
    // switch's old 'break': the next instruction in the same step.
    static void * const DRAM_ATTR dispatch[39] = { &&op_0, &&op_1, &&op_2, &&op_3, &&op_4, &&op_5, &&op_6, &&op_7, &&op_8, &&op_9, &&op_10, &&op_11, &&op_12, &&op_13, &&op_14, &&op_15, &&op_16, &&op_17, &&op_18, &&op_19, &&op_20, &&op_21, &&op_22, &&op_23, &&op_24, &&op_25, &&op_26, &&op_27, &&op_28, &&op_29, &&op_30, &&op_31, &&op_32, &&op_33, &&op_34, &&op_35, &&op_36, &&op_37, &&op_38 };
    goto *dispatch[optcodes[*opcode_stream]];
    {

      // SEG ES
      // SEG CS
      // SEG SS
      // SEG DS
      // opcodes 0x26, 0x2e, 0x36, 0x3e
      op_1:
        seg_override_en = 2;
        seg_override    = ex_data[*opcode_stream];
        if (rep_override_en)
          ++rep_override_en;
        ++reg_ip;
        goto next;

      // JO
      // JNO
      // JB/JNAE/JC
      // JAE/JNB/JNC
      // JE/JZ
      // JNE/JNZ
      // JBE/JNA
      // JA/JNBE
      // JS
      // JNS
      // JP/JPE
      // JNP/JPO
      // JL/JNGE
      // JGE/JNL
      // JLE/JNG
      // JG/JNLE
      // opcodes 0x70 ... 0x7f
      op_2:
      {
        int32_t inv = *opcode_stream & 1; // inv is the invert flag, e.g. 1 means JNAE, whereas 0 means JAE
        int32_t idx = (*opcode_stream >> 1) & 7;
        reg_ip += 2 + (int8_t)opcode_stream[1] * (inv ^ (flags[jxx_dec_a[idx]] || flags[jxx_dec_b[idx]] || flags[jxx_dec_c[idx]] ^ flags[jxx_dec_d[idx]]));
        goto next;
      }

      // JMP disp8
      // opcode 0xeb
      op_3:
        reg_ip += 2 + (int8_t)opcode_stream[1];
        goto next;

      // CLC|STC|CLI|STI|CLD|STD
      // opcodes 0xf8 ... 0xfd
      op_4:
      {
        static DRAM_ATTR int16_t FADDR[3] = { CF_ADDR, IF_ADDR, DF_ADDR };
        flags[FADDR[(*opcode_stream >> 1) & 3]] = *opcode_stream & 1;
        ++reg_ip;
        continue;  // reloop, this is required to inhibit interrupt until next instruction (actually should be just for STI...)
      }

      // JCXZ
      // opcode 0xe3
      op_5:
        reg_ip += 2 + !regs16[REG_CX] * (int8_t)opcode_stream[1];
        goto next;

      // CALL disp16
      // opcode 0xe8
      op_6:
      {
        uint16_t pIP = reg_ip + 3;
        reg_ip = pIP + *(uint16_t*)(opcode_stream + 1);
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = pIP;
        goto next;
      }

      // RET (intrasegment)
      // opcode 0xc3
      op_7:
        reg_ip = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        regs16[REG_SP] += 2;
        goto next;

      // POP reg
      // opcodes 0x58 ... 0x5f
      op_8:
        regs16[REG_SP] += 2;  // SP may be read from stack, so we have to increment here
        regs16[*opcode_stream & 7] = MEM16(16 * regs16[REG_SS] + (uint16_t)(regs16[REG_SP] - 2));
        ++reg_ip;
        goto next;

      // PUSH reg
      // opcodes 0x50 ... 0x57
      op_9:
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[*opcode_stream & 7];
        ++reg_ip;
        goto next;

      // MOV reg8, data8
      // opcodes 0xb0 ... 0xb7
      op_10:
        regs8[((*opcode_stream >> 2) & 1) + (*opcode_stream & 3) * 2] = *(opcode_stream + 1);
        reg_ip += 2;
        goto next;

      // MOV reg16, data16
      // opcodes 0xb8 ... 0xbf
      op_11:
        regs16[*opcode_stream & 0x7] = *(uint16_t*)(opcode_stream + 1);
        reg_ip += 3;
        goto next;

      // POP ES
      // POP SS
      // POP DS
      // opcodes 0x07 0x17 0x1f
      op_12:
        regs16[REG_ES + (*opcode_stream >> 3)] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        regs16[REG_SP] += 2;
        ++reg_ip;
        continue; // reloop, this is required to inhibit interrupt until next instruction (actually just for POP SS)

      // PUSH ES
      // PUSH CS (80186)
      // PUSH SS
      // PUSH DS
      // opcodes 0x06 0x0e 0x16 0x1e
      op_13:
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_ES + (*opcode_stream >> 3)];
        ++reg_ip;
        goto next;

      // REP / REPE / REPNE / REPNZ / REPZ
      // opcodes 0xf2 0xf3
      op_14:
        rep_override_en = 2;
        rep_mode = *opcode_stream & 1;
        if (seg_override_en)
          ++seg_override_en;
        ++reg_ip;
        goto next;

      // INT imm8
      // opcode 0xcd
      op_15:
        reg_ip += 2;
        pc_interrupt(opcode_stream[1]);
        goto next;

      // IRET
      // opcode 0xcf
      op_16:
      {
        uint16_t * stack = &MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        reg_ip         = stack[0];
        regs16[REG_CS] = stack[1];
        set_flags(stack[2]);
        regs16[REG_SP] += 6;
        goto next;
      }

      // RETF (intersegment RET)
      // opcode 0xcb
      op_17:
      {
        uint16_t * stack = &MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        reg_ip         = stack[0];
        regs16[REG_CS] = stack[1];
        regs16[REG_SP] += 4;
        goto next;
      }

      // MOV sreg, r/m16 (0x8e). The 8088 decodes two bits of the reg field: 4..7 are ES..DS again.
      // Then the next instruction in the same step, as the 8086 takes no interrupt after a segment
      // register load (which is what keeps MOV SS, xx / MOV SP, xx safe).
      op_18:
      {
        FastRM rm = decodeRM(opcode_stream + 1, true);
        regs16[REG_ES + ((opcode_stream[1] >> 3) & 3)] = rm.reg ? regs16[rm.idx] : RMEM16(rm.addr);
        reg_ip += 1 + rm.len;
        continue;
      }

      // ESPPC: the fast paths below were measured as frequent in games (the monitor's 'y'
      // profile) and cost 450-550 host cycles through stepEx's generic decoding. They set exactly
      // what stepEx sets, verified against the SingleStepTests 8088 suite (tools/cputest).

      // INC reg16
      // DEC reg16
      // opcodes 0x40 ... 0x4f
      op_19:
      {
        uint16_t & r = regs16[*opcode_stream & 7];
        uint16_t   v = r;
        uint16_t   res;
        if (*opcode_stream & 8) {
          res     = v - 1;
          FLAG_OF = (v == 0x8000);
          FLAG_AF = ((v & 0xf) == 0);       // borrow into bit 4
        } else {
          res     = v + 1;
          FLAG_OF = (v == 0x7fff);
          FLAG_AF = ((v & 0xf) == 0xf);     // carry out of bit 3
        }
        r       = res;                      // CF is left alone, as INC and DEC do
        FLAG_SF = res >> 15;
        FLAG_ZF = !res;
        FLAG_PF = parity[(uint8_t) res];
        ++reg_ip;
        goto next;
      }

      // MOV r/m8, reg8     (0x88)
      // MOV r/m16, reg16   (0x89)
      // MOV reg8, r/m8     (0x8a)
      // MOV reg16, r/m16   (0x8b)
      op_21:
      {
        uint8_t op  = *opcode_stream;
        bool    w   = op & 1;
        FastRM  rm  = decodeRM(opcode_stream + 1, w);
        uint8_t r   = (opcode_stream[1] >> 3) & 7;
        uint8_t ri  = w ? r : ((2 * r + r / 4) & 7);
        if (op & 2) {                       // to the register
          if (w)
            regs16[ri] = rm.reg ? regs16[rm.idx] : RMEM16(rm.addr);
          else
            regs8[ri]  = rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr);
        } else {                            // to r/m
          if (w) {
            if (rm.reg) regs16[rm.idx] = regs16[ri]; else WMEM16(rm.addr, regs16[ri]);
          } else {
            if (rm.reg) regs8[rm.idx] = regs8[ri];   else WMEM8(rm.addr, regs8[ri]);
          }
        }
        reg_ip += 1 + rm.len;
        goto next;
      }

      // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP r/m8, imm8           (0x80)
      // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP r/m16, imm16         (0x81)
      // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP r/m16, imm8 (signed) (0x83)
      op_22:
      {
        uint8_t  op  = *opcode_stream;
        bool     w   = op & 1;
        FastRM   rm  = decodeRM(opcode_stream + 1, w);
        int      alu = (opcode_stream[1] >> 3) & 7;
        uint8_t const * imm = opcode_stream + 1 + rm.len;
        uint32_t b   = (op == 0x81) ? (uint32_t)(imm[0] | (imm[1] << 8))
                     : (op == 0x83) ? (uint32_t)(uint16_t)(int8_t) imm[0]
                     : (uint32_t) imm[0];
        uint32_t a   = w ? (rm.reg ? regs16[rm.idx] : RMEM16(rm.addr))
                         : (rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr));
        uint32_t res = fastALU(alu, a, b, w);
        if (alu != 7) {                     // CMP writes nothing back
          if (w) {
            if (rm.reg) regs16[rm.idx] = res; else WMEM16(rm.addr, res);
          } else {
            if (rm.reg) regs8[rm.idx] = res;  else WMEM8(rm.addr, res);
          }
        }
        reg_ip += 1 + rm.len + (op == 0x81 ? 2 : 1);
        goto next;
      }

      // ROL|ROR|RCL|RCR|SHL|SHR|SAR r/m8, 1   (0xd0)
      // ROL|ROR|RCL|RCR|SHL|SHR|SAR r/m16, 1  (0xd1)
      // ROL|ROR|RCL|RCR|SHL|SHR|SAR r/m8, CL  (0xd2)
      // ROL|ROR|RCL|RCR|SHL|SHR|SAR r/m16, CL (0xd3)
      // By CL the 8088 repeats the single step CL times, all of CL (the 80186 takes it modulo 32,
      // as stepEx does), and the flags are those of the last step; with CL = 0 nothing changes.
      op_23:
      {
        int sh = (opcode_stream[1] >> 3) & 7;
        if (sh == 6) {                      // undocumented, stepEx knows what it does
          stepEx(opcode_stream);
          goto next;
        }
        bool     w   = *opcode_stream & 1;
        FastRM   rm  = decodeRM(opcode_stream + 1, w);
        int      n   = (*opcode_stream & 2) ? regs8[REG_CL] : 1;
        if (n == 0) {
          reg_ip += 1 + rm.len;
          goto next;
        }
        uint32_t v   = w ? (rm.reg ? regs16[rm.idx] : RMEM16(rm.addr))
                         : (rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr));
        uint32_t res = v;
        do
          res = fastShift1(sh, res, w);
        while (--n);
        if (w) {
          if (rm.reg) regs16[rm.idx] = res; else WMEM16(rm.addr, res);
        } else {
          if (rm.reg) regs8[rm.idx] = res;  else WMEM8(rm.addr, res);
        }
        reg_ip += 1 + rm.len;
        goto next;
      }

      // INC r/m8 (0xfe /0), DEC r/m8 (0xfe /1); anything else in the group goes to stepEx
      op_24:
      {
        int sub = (opcode_stream[1] >> 3) & 7;
        if (sub > 1) {
          stepEx(opcode_stream);
          goto next;
        }
        FastRM  rm  = decodeRM(opcode_stream + 1, false);
        uint8_t v   = rm.reg ? regs8[rm.idx] : RMEM8(rm.addr);
        uint8_t res;
        if (sub) {
          res     = v - 1;
          FLAG_OF = (v == 0x80);
          FLAG_AF = ((v & 0xf) == 0);
        } else {
          res     = v + 1;
          FLAG_OF = (v == 0x7f);
          FLAG_AF = ((v & 0xf) == 0xf);
        }
        if (rm.reg) regs8[rm.idx] = res; else WMEM8(rm.addr, res);
        FLAG_SF = res >> 7;
        FLAG_ZF = !res;
        FLAG_PF = parity[res];
        reg_ip += 1 + rm.len;
        goto next;
      }

      // MOV r/m8, imm8 (0xc6 /0), MOV r/m16, imm16 (0xc7 /0)
      op_25:
      {
        if ((opcode_stream[1] >> 3) & 7) {  // not /0: whatever stepEx makes of it
          stepEx(opcode_stream);
          goto next;
        }
        bool   w  = *opcode_stream & 1;
        FastRM rm = decodeRM(opcode_stream + 1, w);
        uint8_t const * imm = opcode_stream + 1 + rm.len;
        if (w) {
          uint16_t v = imm[0] | (imm[1] << 8);
          if (rm.reg) regs16[rm.idx] = v; else WMEM16(rm.addr, v);
        } else {
          if (rm.reg) regs8[rm.idx] = imm[0]; else WMEM8(rm.addr, imm[0]);
        }
        reg_ip += 1 + rm.len + (w ? 2 : 1);
        goto next;
      }

      // MOVSB MOVSW (0xa4 0xa5), STOSB STOSW (0xaa 0xab), LODSB LODSW (0xac 0xad), with or without
      // REP: the whole repetition in this one step, as stepEx did it. Without the decoding these
      // took 525-725 host cycles each, measured; they are frequent in copy and fill loops.
      op_26:
      {
        uint8_t  op   = *opcode_stream;
        bool     w    = op & 1;
        int      kind = op < 0xa6 ? 0 : (op < 0xac ? 1 : 2);     // MOVS, STOS, LODS
        int32_t  seg  = seg_override_en ? seg_override : REG_DS;
        int32_t  size = w ? 2 : 1;
        int32_t  n    = rep_override_en ? regs16[REG_CX] : 1;

        // REP MOVSB/STOSB forwards over plain RAM -- no video window, SI and DI not wrapping --
        // straight through the pointers, strictly byte after byte like the 8086, so an overlapping
        // copy repeats its pattern exactly as the loop below would
        if (rep_override_en && !FLAG_DF && !w && kind < 2 && n > 0) {
          uint32_t dst = 16 * regs16[REG_ES] + regs16[REG_DI];
          uint32_t src = 16 * regs16[seg] + regs16[REG_SI];
          bool plain = regs16[REG_DI] + n <= 0x10000 && dst + n <= (uint32_t) s_state.videoMemStart &&
                       (kind == 1 || (regs16[REG_SI] + n <= 0x10000 && src + n <= (uint32_t) s_state.videoMemStart));
          if (plain) {
            uint8_t * d = s_state.memory + dst;
            if (kind == 0) {
              uint8_t const * s = s_state.memory + src;
              for (int32_t i = 0; i < n; ++i)
                d[i] = s[i];
              regs16[REG_SI] += n;
            } else {
              uint8_t v = regs8[REG_AL];
              for (int32_t i = 0; i < n; ++i)
                d[i] = v;
            }
            PSRAM_WORKAROUND2;
            regs16[REG_DI] += n;
            regs16[REG_CX]  = 0;
            ++reg_ip;
            goto next;
          }
        }

        int32_t d = FLAG_DF ? -size : size;
        for (int32_t i = n; i; --i) {
          if (w) {
            uint16_t v = (kind == 1) ? regs16[REG_AX] : RMEM16(16 * regs16[seg] + regs16[REG_SI]);
            if (kind < 2) {
              WMEM16(16 * regs16[REG_ES] + regs16[REG_DI], v);
              PSRAM_WORKAROUND2;
            } else
              regs16[REG_AX] = v;
          } else {
            uint8_t v = (kind == 1) ? regs8[REG_AL] : RMEM8(16 * regs16[seg] + regs16[REG_SI]);
            if (kind < 2) {
              WMEM8(16 * regs16[REG_ES] + regs16[REG_DI], v);
              PSRAM_WORKAROUND2;
            } else
              regs8[REG_AL] = v;
          }
          if (kind != 1)
            regs16[REG_SI] += d;
          if (kind != 2)
            regs16[REG_DI] += d;
        }
        if (rep_override_en)
          regs16[REG_CX] = 0;
        ++reg_ip;
        goto next;
      }

      // IN AL/AX, imm8      (0xe4 0xe5)
      // OUT imm8, AL/AX     (0xe6 0xe7)
      // IN AL/AX, DX        (0xec 0xed)
      // OUT DX, AL/AX       (0xee 0xef)
      // A word is two byte accesses, port and port + 1, as in stepEx.
      op_20:
      {
        uint8_t op   = *opcode_stream;
        int32_t port = (op & 8) ? regs16[REG_DX] : opcode_stream[1];
        if (op & 2) {
          s_state.writePort(s_state.context, port, regs8[REG_AL]);
          if (op & 1)
            s_state.writePort(s_state.context, port + 1, regs8[REG_AH]);
        } else {
          regs8[REG_AL] = s_state.readPort(s_state.context, port);
          if (op & 1)
            regs8[REG_AH] = s_state.readPort(s_state.context, port + 1);
        }
        reg_ip += (op & 8) ? 1 : 2;
        goto next;
      }

      // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP r/m, reg and reg, r/m, 8 and 16 bits
      // opcodes 0x00 ... 0x3b, the first four of each eight
      op_27:
      {
        uint8_t op  = *opcode_stream;
        bool    w   = op & 1;
        int     alu = op >> 3;
        FastRM  rm  = decodeRM(opcode_stream + 1, w);
        uint8_t r   = (opcode_stream[1] >> 3) & 7;
        uint8_t ri  = w ? r : ((2 * r + r / 4) & 7);
        uint32_t m  = w ? (rm.reg ? regs16[rm.idx] : RMEM16(rm.addr))
                        : (rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr));
        uint32_t g  = w ? regs16[ri] : regs8[ri];
        bool    toR = op & 2;               // to the register, else to r/m
        uint32_t res = fastALU(alu, toR ? g : m, toR ? m : g, w);
        if (alu != 7) {
          if (toR) {
            if (w) regs16[ri] = res; else regs8[ri] = res;
          } else if (w) {
            if (rm.reg) regs16[rm.idx] = res; else WMEM16(rm.addr, res);
          } else {
            if (rm.reg) regs8[rm.idx] = res;  else WMEM8(rm.addr, res);
          }
        }
        reg_ip += 1 + rm.len;
        goto next;
      }

      // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP AL, imm8 and AX, imm16 (0x04 ... 0x3d, the fifth and sixth
      // of each eight); TEST AL, imm8 (0xa8) and AX, imm16 (0xa9), an AND that writes nothing
      op_28:
      {
        uint8_t  op   = *opcode_stream;
        bool     w    = op & 1;
        bool     test = op >= 0xa8;
        int      alu  = test ? 4 : op >> 3;
        uint32_t b    = w ? (uint32_t)(opcode_stream[1] | (opcode_stream[2] << 8)) : opcode_stream[1];
        uint32_t res  = fastALU(alu, w ? regs16[REG_AX] : regs8[REG_AL], b, w);
        if (alu != 7 && !test) {
          if (w) regs16[REG_AX] = res; else regs8[REG_AL] = res;
        }
        reg_ip += w ? 3 : 2;
        goto next;
      }

      // TEST r/m8, reg8 (0x84), TEST r/m16, reg16 (0x85)
      op_34:
      {
        bool    w  = *opcode_stream & 1;
        FastRM  rm = decodeRM(opcode_stream + 1, w);
        uint8_t r  = (opcode_stream[1] >> 3) & 7;
        uint32_t a = w ? (rm.reg ? regs16[rm.idx] : RMEM16(rm.addr))
                       : (rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr));
        fastALU(4, a, w ? regs16[r] : regs8[(2 * r + r / 4) & 7], w);
        reg_ip += 1 + rm.len;
        goto next;
      }

      // TEST r/m, imm (0xf6 /0, 0xf7 /0), NOT (/2), NEG (/3); MUL, IMUL, DIV, IDIV and the
      // undocumented /1 go to stepEx
      op_29:
      {
        int sub = (opcode_stream[1] >> 3) & 7;
        if (sub == 1 || sub > 3) {
          stepEx(opcode_stream);
          goto next;
        }
        bool     w  = *opcode_stream & 1;
        FastRM   rm = decodeRM(opcode_stream + 1, w);
        uint32_t v  = w ? (rm.reg ? regs16[rm.idx] : RMEM16(rm.addr))
                        : (rm.reg ? regs8[rm.idx]  : RMEM8(rm.addr));
        if (sub == 2) {                     // NOT, no flags
          v = ~v & (w ? 0xffff : 0xff);
        } else {                            // TEST v, imm, or NEG with the flags of 0 - v
          uint8_t const * imm = opcode_stream + 1 + rm.len;
          uint32_t b = w ? (uint32_t)(imm[0] | (imm[1] << 8)) : imm[0];
          uint32_t r = fastALU(sub ? 5 : 4, sub ? 0 : v, sub ? v : b, w);
          if (sub == 0) {
            reg_ip += 1 + rm.len + (w ? 2 : 1);
            goto next;
          }
          v = r;
        }
        uint32_t res = v;
        if (w) {
          if (rm.reg) regs16[rm.idx] = res; else WMEM16(rm.addr, res);
        } else {
          if (rm.reg) regs8[rm.idx] = res;  else WMEM8(rm.addr, res);
        }
        reg_ip += 1 + rm.len;
        goto next;
      }

      // INC r/m16 (0xff /0), DEC r/m16 (/1), CALL near r/m16 (/2), JMP near r/m16 (/4), PUSH r/m16
      // (/6); the far forms and the undocumented /7 go to stepEx
      op_30:
      {
        int sub = (opcode_stream[1] >> 3) & 7;
        if (sub == 3 || sub == 5 || sub == 7) {
          stepEx(opcode_stream);
          goto next;
        }
        FastRM rm = decodeRM(opcode_stream + 1, true);
        if (sub < 2) {
          uint16_t v = rm.reg ? regs16[rm.idx] : RMEM16(rm.addr);
          uint16_t res;
          if (sub) {
            res     = v - 1;
            FLAG_OF = (v == 0x8000);
            FLAG_AF = ((v & 0xf) == 0);
          } else {
            res     = v + 1;
            FLAG_OF = (v == 0x7fff);
            FLAG_AF = ((v & 0xf) == 0xf);
          }
          if (rm.reg) regs16[rm.idx] = res; else WMEM16(rm.addr, res);
          FLAG_SF = res >> 15;
          FLAG_ZF = !res;
          FLAG_PF = parity[(uint8_t) res];
          reg_ip += 1 + rm.len;
          goto next;
        }
        if (sub == 6) {                     // PUSH: of SP itself the value after the decrement
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = rm.reg ? regs16[rm.idx] : RMEM16(rm.addr);
          reg_ip += 1 + rm.len;
          goto next;
        }
        uint16_t to = rm.reg ? regs16[rm.idx] : RMEM16(rm.addr);
        if (sub == 2) {                     // CALL
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = reg_ip + 1 + rm.len;
        }
        reg_ip = to;
        goto next;
      }

      // POP r/m16 (0x8f /0): the address from SP before it moves
      op_35:
      {
        if ((opcode_stream[1] >> 3) & 7) {
          stepEx(opcode_stream);
          goto next;
        }
        FastRM rm = decodeRM(opcode_stream + 1, true);
        regs16[REG_SP] += 2;
        uint16_t v = MEM16(16 * regs16[REG_SS] + (uint16_t)(regs16[REG_SP] - 2));
        if (rm.reg) regs16[rm.idx] = v; else WMEM16(rm.addr, v);
        reg_ip += 1 + rm.len;
        goto next;
      }

      // MOV AL, [moffs] (0xa0), MOV AX, [moffs] (0xa1), MOV [moffs], AL (0xa2), MOV [moffs], AX (0xa3)
      op_31:
      {
        uint8_t op   = *opcode_stream;
        int32_t addr = 16 * regs16[seg_override_en ? seg_override : REG_DS] + (uint16_t)(opcode_stream[1] | (opcode_stream[2] << 8));
        if (op & 2) {
          if (op & 1) WMEM16(addr, regs16[REG_AX]); else WMEM8(addr, regs8[REG_AL]);
        } else {
          if (op & 1) regs16[REG_AX] = RMEM16(addr); else regs8[REG_AL] = RMEM8(addr);
        }
        reg_ip += 3;
        goto next;
      }

      // CALL far ptr16:16 (0x9a)
      op_32:
      {
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_CS];
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = reg_ip + 5;
        reg_ip         = opcode_stream[1] | (opcode_stream[2] << 8);
        regs16[REG_CS] = opcode_stream[3] | (opcode_stream[4] << 8);
        goto next;
      }

      // JMP near rel16 (0xe9)
      op_33:
        reg_ip += 3 + (opcode_stream[1] | (opcode_stream[2] << 8));
        goto next;

      // RET imm16 (0xc2), RETF imm16 (0xca)
      op_36:
      {
        uint16_t   n     = opcode_stream[1] | (opcode_stream[2] << 8);
        uint16_t * stack = &MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        reg_ip = stack[0];
        if (*opcode_stream & 8) {
          regs16[REG_CS]  = stack[1];
          regs16[REG_SP] += 4 + n;
        } else
          regs16[REG_SP] += 2 + n;
        goto next;
      }

      // MOV r/m16, sreg (0x8c), reg field decoded as for 0x8e
      op_37:
      {
        FastRM   rm = decodeRM(opcode_stream + 1, true);
        uint16_t v  = regs16[REG_ES + ((opcode_stream[1] >> 3) & 3)];
        if (rm.reg) regs16[rm.idx] = v; else WMEM16(rm.addr, v);
        reg_ip += 1 + rm.len;
        goto next;
      }

      // XLAT (0xd7)
      op_38:
        regs8[REG_AL] = RMEM8(16 * regs16[seg_override_en ? seg_override : REG_DS] + (uint16_t)(regs8[REG_AL] + regs16[REG_BX]));
        ++reg_ip;
        goto next;

      op_0:
        stepEx(opcode_stream);
        goto next;

    }
    
  };


  // ESPPC: the next instruction of the batch, the interrupt checks above included
next:
  if (--count > 0)
    goto begin;
}


// ESPPC: in IRAM too, for the same reason as step(). This is the instruction interpreter
// proper, about 21 KB, and the largest single piece of code in IRAM.
void IRAM_ATTR i8086::stepEx(uint8_t const * opcode_stream)
{
  set_opcode(*opcode_stream);

  // Extract fields from instruction
  uint8_t i_reg4bit = raw_opcode_id & 7;
  i_w = i_reg4bit & 1;
  i_d = i_reg4bit / 2 & 1;

  // Extract instruction data fields
  uint16_t i_data0 = opcode_stream[1] | (opcode_stream[2] << 8);
  uint16_t i_data1 = (i_data0 >> 8) | (opcode_stream[3] << 8);
  uint16_t i_data2 = (i_data1 >> 8) | (opcode_stream[4] << 8);

  uint8_t i_mod = 0, i_rm = 0, i_reg = 0;
  int32_t op_result = 0;
  int32_t rm_addr = 0;

  bool calcIP = true;

  // i_mod_size > 0 indicates that opcode uses i_mod/i_rm/i_reg, so decode them
  if (i_mod_size) {
    i_mod = (i_data0 & 0xFF) >> 6;
    i_rm  = i_data0 & 7;
    i_reg = i_data0 / 8 & 7;

    if ((!i_mod && i_rm == 6) || (i_mod == 2))
      i_data2 = (i_data2 >> 8) | (opcode_stream[5] << 8);
    else if (i_mod != 1)
      i_data2 = i_data1;
    else // If i_mod is 1, operand is (usually) 8 bits rather than 16 bits
      i_data1 = (int8_t) i_data1;

    int idx = 4 * !i_mod;
    op_to_addr = rm_addr = i_mod < 3 ? 16 * regs16[seg_override_en ? seg_override : instr_table_lookup[idx + 3][i_rm]] + (uint16_t)(regs16[instr_table_lookup[idx + 1][i_rm]] + instr_table_lookup[idx + 2][i_rm] * i_data1 + regs16[instr_table_lookup[idx][i_rm]]) : (regs_offset + (i_w ? 2 * i_rm : (2 * i_rm + i_rm / 4) & 7));
    op_from_addr = regs_offset + (i_w ? 2 * i_reg : (2 * i_reg + i_reg / 4) & 7);
    if (i_d) {
      auto t = op_from_addr;
      op_from_addr = rm_addr;
      op_to_addr = t;
    }
  }

  // Instruction execution unit
  // ESPPC: dispatched through a table of label addresses in DRAM, as in step() and for the same
  // reason: the IDF's -fno-jump-tables made the switch here a tree of compares. 'goto ex_done' is
  // the switch's old 'break', on to the IP update and the flags below.
  static void * const DRAM_ATTR dispatch[73] = { &&ex_default, &&ex_default, &&ex_2, &&ex_default, &&ex_default, &&ex_5, &&ex_6, &&ex_default, &&ex_default, &&ex_default, &&ex_10, &&ex_default, &&ex_12, &&ex_13, &&ex_14, &&ex_default, &&ex_16, &&ex_17, &&ex_18, &&ex_19, &&ex_20, &&ex_21, &&ex_22, &&ex_default, &&ex_24, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_30, &&ex_31, &&ex_32, &&ex_33, &&ex_34, &&ex_35, &&ex_36, &&ex_37, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default, &&ex_default };
  goto *dispatch[xlat_opcode_id];
  {
    ex_2: // INC|DEC regs16
    {
      i_w = 1;
      i_d = 0;
      i_reg = i_reg4bit;
      int idx = 4 * !i_mod;
      op_to_addr = rm_addr = i_mod < 3 ? 16 * regs16[seg_override_en ? seg_override : instr_table_lookup[idx + 3][i_rm]] + (uint16_t)(regs16[instr_table_lookup[idx + 1][i_rm]] + instr_table_lookup[idx + 2][i_rm] * i_data1 + regs16[instr_table_lookup[idx][i_rm]]) : (regs_offset + 2 * i_rm);
      op_from_addr = regs_offset + 2 * i_reg;
      i_reg = extra;
    } // not break!
    ex_5: // INC|DEC|JMP|CALL|PUSH
      if (i_reg < 2) {
        // INC|DEC
        if (i_w) {
          op_dest = RMEM16(op_from_addr);
          op_result = WMEM16(op_from_addr, (uint16_t)op_dest + 1 - 2 * i_reg);
        } else {
          op_dest = RMEM8(op_from_addr);
          op_result = WMEM8(op_from_addr, (uint16_t)op_dest + 1 - 2 * i_reg);
        }
        op_source = 1;
        set_AF_OF_arith(op_result);
        FLAG_OF = (bool)(op_dest + 1 - i_reg == 1 << (8 * (i_w + 1) - 1));
        if (xlat_opcode_id == 5) {
          // Decode like ADC (0x10)
          xlat_opcode_id = 0x10;
          set_flags_type = 1;
        }
      } else if (i_reg != 6) {
        // JMP|CALL
        uint16_t jumpTo = i_w ? MEM16(op_from_addr) : MEM8(op_from_addr);
        if (i_reg - 3 == 0) {
          // CALL (far)
          i_w = 1;
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_CS];
        }
        if (i_reg & 2) {
          // CALL (near or far)
          i_w = 1;
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = (reg_ip + 2 + i_mod * (i_mod != 3) + 2 * (!i_mod && i_rm == 6));
        }
        if (i_reg & 1) {
          // JMP|CALL (far)
          PSRAM_WORKAROUND2
          regs16[REG_CS] = MEM16(op_from_addr + 2);
        }
        reg_ip = jumpTo;
        return; // no calc IP, no flags
      } else {
        // PUSH
        i_w = 1;
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = RMEM16(rm_addr);
      }
      goto ex_done;
    ex_6: // TEST r/m, imm16 / NOT|NEG|MUL|IMUL|DIV|IDIV reg
      op_to_addr = op_from_addr;

      switch (i_reg) {
        case 0: // TEST
          // Decode like AND (0x20)
          raw_opcode_id = 0x20;
          set_flags_type = 5;

          reg_ip += i_w + 1;
          if (i_w) {
            op_dest = RMEM16(op_to_addr);
            op_source = (uint16_t)i_data2;
            op_result = (uint16_t)(op_dest & op_source);
          } else {
            op_dest = RMEM8(op_to_addr);
            op_source = (uint8_t)i_data2;
            op_result = (uint8_t)(op_dest & op_source);
          }
          break;
        case 2: // NOT
          if (i_w)
            WMEM16(op_to_addr, ~RMEM16(op_from_addr));
          else
            WMEM8(op_to_addr, ~RMEM8(op_from_addr));
          break;
        case 3: // NEG
          if (i_w)
            op_result = WMEM16(op_to_addr, -(op_source = RMEM16(op_from_addr)));
          else
            op_result = WMEM8(op_to_addr, -(op_source = RMEM8(op_from_addr)));
          op_dest = 0;

          // Decode like SUB (0x28)
          raw_opcode_id  = 0x28;
          set_flags_type = 3;

          FLAG_CF = op_result > op_dest;
          break;
        case 4: // MUL
          raw_opcode_id  = 0x10;
          set_flags_type = 1;
          if (i_w) {
            regs16[REG_DX] = (op_result = RMEM16(rm_addr) * regs16[REG_AX]) >> 16;
            regs16[REG_AX] = op_result;
            FLAG_OF = FLAG_CF = (bool)(op_result - (uint16_t)op_result);
          } else {
            regs16[REG_AX] = op_result = RMEM8(rm_addr) * regs8[REG_AL];
            FLAG_OF = FLAG_CF = (bool)(op_result - (uint8_t) op_result);
          }
          break;
        case 5: // IMUL
          raw_opcode_id  = 0x10;
          set_flags_type = 1;
          if (i_w) {
            regs16[REG_DX] = (op_result = (int16_t)RMEM16(rm_addr) * (int16_t)regs16[REG_AX]) >> 16;
            regs16[REG_AX] = op_result;
            FLAG_OF = FLAG_CF = (bool)(op_result - (int16_t)op_result);
          } else {
            regs16[REG_AX] = op_result = (int8_t)RMEM8(rm_addr) * (int8_t)regs8[REG_AL];
            FLAG_OF = FLAG_CF = (bool)(op_result - (int8_t) op_result);
          }
          break;
        case 6: // DIV
        {
          int32_t scratch_int;
          int32_t scratch_uint, scratch2_uint;
          if (i_w) {
            (scratch_int = RMEM16(rm_addr))
            &&
            !(scratch2_uint = (uint32_t)(scratch_uint = (regs16[REG_DX] << 16) + regs16[REG_AX]) / scratch_int, scratch2_uint - (uint16_t) scratch2_uint)
            ?
            regs16[REG_DX] = scratch_uint - scratch_int * (regs16[REG_AX] = scratch2_uint)
            :
            (raiseDivideByZeroInterrupt(), calcIP = false);
          } else {
            (scratch_int = RMEM8(rm_addr))
            &&
            !(scratch2_uint = (uint16_t)(scratch_uint = regs16[REG_AX]) / scratch_int, scratch2_uint - (uint8_t) scratch2_uint)
            ?
            regs8[REG_AH] = scratch_uint - scratch_int * (regs8[REG_AL] = scratch2_uint)
            :
            (raiseDivideByZeroInterrupt(), calcIP = false);
          }
          break;
        }
        case 7: // IDIV
        {
          int32_t scratch_int;
          int32_t scratch2_uint, scratch_uint;
          if (i_w) {
            (scratch_int = (int16_t)RMEM16(rm_addr))
            &&
            !(scratch2_uint = (int)(scratch_uint = (regs16[REG_DX] << 16) + regs16[REG_AX]) / scratch_int, scratch2_uint - (int16_t) scratch2_uint)
            ?
            regs16[REG_DX] = scratch_uint - scratch_int * (regs16[REG_AX] = scratch2_uint)
            :
            (raiseDivideByZeroInterrupt(), calcIP = false);
          } else {
            (scratch_int = (int8_t)RMEM8(rm_addr))
            &&
            !(scratch2_uint = (int16_t)(scratch_uint = regs16[REG_AX]) / scratch_int, scratch2_uint - (int8_t) scratch2_uint)
            ?
            regs8[REG_AH] = scratch_uint - scratch_int * (regs8[REG_AL] = scratch2_uint)
            :
            (raiseDivideByZeroInterrupt(), calcIP = false);
          }
          break;
        }
      };
      goto ex_done;
    // ESPPC: ADD|OR|ADC|SBB|AND|SUB|XOR|CMP in all their forms (cases 7 to 9), MOV AL/AX with
    // [loc] (case 11) and TEST (cases 15 and 47) were here. step() runs all of them on its fast paths now, so they never get
    // here, and the IRAM they took went to those paths.
    ex_10: // LEA reg, r/m | POP r/m (MOV sreg, both ways, is in step())
      if (!i_d) {  // 0x8d
        // LEA
        int idx = 4 * !i_mod;
        regs16[i_reg] = regs16[instr_table_lookup[idx + 1][i_rm]] + instr_table_lookup[idx + 2][i_rm] * i_data1 + regs16[instr_table_lookup[idx][i_rm]];
      } else {  // 0x8f
        // POP
        regs16[REG_SP] += 2;
        WMEM16(rm_addr, MEM16(16 * regs16[REG_SS] + (uint16_t)(-2 + regs16[REG_SP])));
      }
      goto ex_done;
    ex_12: // ROL|ROR|RCL|RCR|SHL|SHR|???|SAR reg/MEM, 1/CL/imm (80186)
    {
      uint16_t scratch2_uint = (1 & (i_w ? (int16_t)RMEM16(rm_addr) : RMEM8(rm_addr)) >> (8 * (i_w + 1) - 1));
      uint16_t scratch_uint  = extra ? // xxx reg/MEM, imm
        (int8_t) i_data1 : // xxx reg/MEM, CL
        i_d ?
        31 & regs8[REG_CL] : // xxx reg/MEM, 1
        1;
      if (scratch_uint) {
        if (i_reg < 4) {
          // Rotate operations
          scratch_uint %= i_reg / 2 + 8 * (i_w + 1);
          scratch2_uint = i_w ? RMEM16(rm_addr) : RMEM8(rm_addr);
        }
        if (i_reg & 1) {
          // Rotate/shift right operations
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest >> scratch_uint);
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest >> (uint8_t)scratch_uint);
          }
        } else {
          // Rotate/shift left operations
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest << scratch_uint);
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest << (uint8_t)scratch_uint);
          }
        }
        if (i_reg > 3) // Shift operations
          set_flags_type = 1; // Shift instructions affect SZP
        if (i_reg > 4) // SHR or SAR
          FLAG_CF = op_dest >> (scratch_uint - 1) & 1;
      }

      switch (i_reg) {
        case 0: // ROL
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest + (op_source = scratch2_uint >> (16 - scratch_uint)));
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest + (op_source = (uint8_t)scratch2_uint >> (8 - scratch_uint)));
          }
          if (scratch_uint) // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_OF = (1 & op_result >> (8 * (i_w + 1) - 1)) ^ (FLAG_CF = op_result & 1);
          break;
        case 1: // ROR
          scratch2_uint &= (1 << scratch_uint) - 1;
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest + (op_source = scratch2_uint << (16 - scratch_uint)));
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest + (op_source = (uint8_t)scratch2_uint << (8 - scratch_uint)));
          }
          if (scratch_uint) // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_OF = (1 & (i_w ? (int16_t)op_result * 2 : op_result * 2) >> (8 * (i_w + 1) - 1)) ^ (FLAG_CF = 1 & (i_w ? (int16_t)op_result : op_result) >> (8 * (i_w + 1) - 1));
            ;
          break;
        case 2: // RCL
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest + (FLAG_CF << (scratch_uint - 1)) + (op_source = scratch2_uint >> (17 - scratch_uint)));
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest + (FLAG_CF << (scratch_uint - 1)) + (op_source = (uint8_t)scratch2_uint >> (9 - scratch_uint)));
          }
          if (scratch_uint) // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_OF = (1 & op_result >> (8 * (i_w + 1) - 1)) ^ (FLAG_CF = (bool)(scratch2_uint & 1 << (8 * (i_w + 1) - scratch_uint)));
          break;
        case 3: // RCR
          if (i_w) {
            op_dest   = RMEM16(rm_addr);
            op_result = WMEM16(rm_addr, (uint16_t)op_dest + (FLAG_CF << (16 - scratch_uint)) + (op_source = scratch2_uint << (17 - scratch_uint)));
          } else {
            op_dest   = RMEM8(rm_addr);
            op_result = WMEM8(rm_addr, (uint8_t)op_dest + (FLAG_CF << (8 - scratch_uint)) + (op_source = (uint8_t)scratch2_uint << (9 - scratch_uint)));
          }
          if (scratch_uint) { // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_CF = (bool)(scratch2_uint & 1 << (scratch_uint - 1));
            FLAG_OF = (1 & op_result >> (8 * (i_w + 1) - 1)) ^ (1 & (i_w ? (int16_t)op_result * 2 : op_result * 2) >> (8 * (i_w + 1) - 1));
          }
          break;
        case 4: // SHL
          if (scratch_uint) // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_OF = (1 & op_result >> (8 * (i_w + 1) - 1)) ^ (FLAG_CF = (1 & (op_dest << (scratch_uint - 1)) >> (8 * (i_w + 1) - 1)));
          break;
        case 5: // SHR
          if (scratch_uint) // fab: zero shift/rotation doesn't change CF or OF ("rotate.asm" and "shift.asm" tests)
            FLAG_OF = 1 & op_dest >> (8 * (i_w + 1) - 1);
          break;
        case 7: // SAR
          scratch_uint < 8 * (i_w + 1) || (FLAG_CF = (bool)scratch2_uint);
          FLAG_OF = 0;
          if (i_w) {
            op_dest      = RMEM16(rm_addr);
            uint16_t u16 = (uint16_t)scratch2_uint * ~(((1 << 16) - 1) >> scratch_uint);
            op_result    = WMEM16(rm_addr, op_dest + (op_source = u16));
          } else {
            op_dest    = RMEM8(rm_addr);
            uint8_t u8 = (uint8_t)scratch2_uint * ~(((1 << 8) - 1) >> scratch_uint);
            op_result  = WMEM8(rm_addr, op_dest + (op_source = u8));
          }
          break;
      };
      goto ex_done;
    }
    ex_13: // LOOPxx
      // i_reg4bit: 0=LOOPNZ, 1=LOOPZ, 2=LOOP
      if (--regs16[REG_CX])
        reg_ip += ((FLAG_ZF ^ !i_reg4bit) | (bool)(i_reg4bit & 2)) * (int8_t) i_data0;
      goto ex_done;
    ex_14: // JMP | CALL int16_t/near
      reg_ip += 3 - i_d;
      if (!i_w) {
        if (i_d) {
          // JMP far
          reg_ip = 0;
          regs16[REG_CS] = i_data2;
        } else {
          // CALL
          i_w = 1;
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = reg_ip;
        }
      }
      reg_ip += i_d && i_w ? (int8_t) i_data0 : i_data0;
      return; // no calc IP, no flags
    ex_16: // XCHG AX, regs16
      if (i_reg4bit != REG_AX) {
        uint16_t t = regs16[REG_AX];
        regs16[REG_AX] = regs16[i_reg4bit];
        regs16[i_reg4bit] = t;
      }
      ++reg_ip;
      return; // no calc ip, no flags
    ex_24: // NOP|XCHG reg, r/m
      if (op_to_addr != op_from_addr) {
        if (i_w) {
          uint16_t t = RMEM16(op_to_addr);
          WMEM16(op_to_addr, RMEM16(op_from_addr));
          WMEM16(op_from_addr, t);
        } else {
          uint16_t t = RMEM8(op_to_addr);
          WMEM8(op_to_addr, RMEM8(op_from_addr));
          WMEM8(op_from_addr, t);
        }
      }
      goto ex_done;
    ex_17: // MOVSx (extra=0)|STOSx (extra=1)|LODSx (extra=2)
    {
      int32_t seg = seg_override_en ? seg_override : REG_DS;
      if (i_w) {
        const int dec = (2 * FLAG_DF - 1) * 2;
        for (int32_t i = rep_override_en ? regs16[REG_CX] : 1; i; --i) {
          uint16_t src = extra & 1 ? regs16[REG_AX] : RMEM16(16 * regs16[seg] + regs16[REG_SI]);
          if (extra < 2) {
            WMEM16(16 * regs16[REG_ES] + regs16[REG_DI], src);
            PSRAM_WORKAROUND2;
          } else
            regs16[REG_AX] = src;
          extra & 1 || (regs16[REG_SI] -= dec);
          extra & 2 || (regs16[REG_DI] -= dec);

        }
      } else {
        const int dec = (2 * FLAG_DF - 1);
        for (int32_t i = rep_override_en ? regs16[REG_CX] : 1; i; --i) {
          uint8_t src = extra & 1 ? regs8[REG_AL] : RMEM8(16 * regs16[seg] + regs16[REG_SI]);
          if (extra < 2) {
            WMEM8(16 * regs16[REG_ES] + regs16[REG_DI], src);
            PSRAM_WORKAROUND2;
          } else
            regs8[REG_AL] = src;
          extra & 1 || (regs16[REG_SI] -= dec);
          extra & 2 || (regs16[REG_DI] -= dec);
        }
      }
      if (rep_override_en)
        regs16[REG_CX] = 0;
      ++reg_ip;
      return; // no calc ip, no flags
    }
    ex_18: // CMPSx (extra=0)|SCASx (extra=1)
    {
      int count = rep_override_en ? regs16[REG_CX] : 1;
      if (count) {
        int incval = (2 * FLAG_DF - 1) * (i_w + 1);
        if (extra) {
          // SCASx
          op_dest = i_w ? regs16[REG_AX] : regs8[REG_AL];
          for (; count; rep_override_en || count--) {
            if (i_w) {
              op_result = op_dest - (op_source = RMEM16(16 * regs16[REG_ES] + regs16[REG_DI]));
            } else {
              op_result = op_dest - (op_source = RMEM8(16 * regs16[REG_ES] + regs16[REG_DI]));
            }
            regs16[REG_DI] -= incval;
            rep_override_en && !(--regs16[REG_CX] && ((!op_result) == rep_mode)) && (count = 0);
          }
        } else {
          // CMPSx
          int scratch2_uint = seg_override_en ? seg_override : REG_DS;
          for (; count; rep_override_en || count--) {
            if (i_w) {
              op_dest   = RMEM16(16 * regs16[scratch2_uint] + regs16[REG_SI]);
              op_result = op_dest - (op_source = RMEM16(16 * regs16[REG_ES] + regs16[REG_DI]));
            } else {
              op_dest   = RMEM8(16 * regs16[scratch2_uint] + regs16[REG_SI]);
              op_result = op_dest - (op_source = RMEM8(16 * regs16[REG_ES] + regs16[REG_DI]));
            }
            regs16[REG_SI] -= incval;
            regs16[REG_DI] -= incval;
            rep_override_en && !(--regs16[REG_CX] && ((!op_result) == rep_mode)) && (count = 0);
          }
        }
        set_flags_type = 3; // set SZP/AO flags
        FLAG_CF = op_result > op_dest;
      };
      ++reg_ip;
      calcIP = false;
      goto ex_done;
    }
    ex_19: // RET / RETF imm16
      reg_ip = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      if (extra) {
        // RETF
        regs16[REG_CS] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
        regs16[REG_SP] += 2;
      }
      regs16[REG_SP] += i_data0;
      return; // no calc ip, no flags
    ex_20: // MOV r/m, immed
      if (i_w) {
        WMEM16(op_from_addr, i_data2);
      } else {
        WMEM8(op_from_addr, i_data2);
      }
      goto ex_done;
    ex_21: // IN AL/AX, DX/imm8
    {
      int32_t port = extra ? regs16[REG_DX] : (uint8_t) i_data0;
      regs8[REG_AL] = s_state.readPort(s_state.context, port);
      if (i_w)
        regs8[REG_AH] = s_state.readPort(s_state.context, port + 1);
      goto ex_done;
    }
    ex_22: // OUT DX/imm8, AL/AX
    {
      int32_t port = extra ? regs16[REG_DX] : (uint8_t) i_data0;
      s_state.writePort(s_state.context, port, regs8[REG_AL]);
      if (i_w)
        s_state.writePort(s_state.context, port + 1, regs8[REG_AH]);
      goto ex_done;
    }
    ex_30: // CBW
      regs8[REG_AH] = -(1 & (i_w ? * (int16_t *) & regs8[REG_AL] : regs8[REG_AL]) >> (8 * (i_w + 1) - 1));
      goto ex_done;
    ex_31: // CWD
      regs16[REG_DX] = -(1 & (i_w ? * (int16_t *) & regs16[REG_AX] : regs16[REG_AX]) >> (8 * (i_w + 1) - 1));
      goto ex_done;
    ex_32: // CALL FAR imm16:imm16
    {
      regs16[REG_SP] -= 4;
      uint16_t * stack = &MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      stack[1] = regs16[REG_CS];
      stack[0] = reg_ip + 5;
      regs16[REG_CS]  = i_data2;
      reg_ip          = i_data0;
      return; // no calc ip, no flags
    }
    ex_33: // PUSHF
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = make_flags();
      ++reg_ip;
      return; // no calc ip, no flags
    ex_34: // POPF
      regs16[REG_SP] += 2;
      set_flags(MEM16(16 * regs16[REG_SS] + (uint16_t)(-2 + regs16[REG_SP])));
      ++reg_ip;
      return; // no calc ip, no flags
    ex_35: // SAHF
      set_flags((make_flags() & 0xFF00) + regs8[REG_AH]);
      goto ex_done;
    ex_36: // LAHF
      regs8[REG_AH] = make_flags();
      goto ex_done;
    ex_37: // LES|LDS reg, r/m
      i_w = i_d = 1;
      regs16[i_reg] = RMEM16(rm_addr);
      regs16[extra / 2] = RMEM16(rm_addr + 2);
      goto ex_done;
    // ESPPC: everything rare lives in stepExCold(), in flash (see there)
    ex_default:
      if (stepExCold(opcode_stream, i_data0, i_data2, op_result))
        return; // no calc ip, no flags
      goto ex_done;
  }
  ex_done:

  // Increment instruction pointer by computed instruction length.
  if (calcIP)
    reg_ip += (i_mod * (i_mod != 3) + 2 * (!i_mod && i_rm == 6)) * i_mod_size + base_size[raw_opcode_id] + i_w_adder[raw_opcode_id] * (i_w + 1);

  // If instruction needs to update SF, ZF and PF, set them as appropriate
  if (set_flags_type & 1) {
    FLAG_SF = (1 & op_result >> (8 * (i_w + 1) - 1));
    FLAG_ZF = !op_result;
    FLAG_PF = parity[(uint8_t) op_result];

    // If instruction is an arithmetic or logic operation, also set AF/OF/CF as appropriate.
    if (set_flags_type & 2)
      set_AF_OF_arith(op_result);
    else if (set_flags_type & 4)
      FLAG_CF = FLAG_OF = 0;
  }

}


// ESPPC: the instructions no game runs in a loop -- BCD adjustments, INT 3, INTO, SALC, CMC,
// LOCK, HLT, the 80186 additions, the 8087 message and the unsupported opcode fallback -- in
// flash rather than IRAM, which stepEx() had filled with 21 KB and which the fast paths in step()
// need. noinline, or the compiler would put it straight back into stepEx. Returns true where
// stepEx() must return at once (no IP update, no flags), false to carry on with its tail.
bool __attribute__((noinline)) i8086::stepExCold(uint8_t const * opcode_stream, uint16_t i_data0, uint16_t i_data2, int32_t & op_result)
{
  switch (xlat_opcode_id) {
    case 28: // DAA/DAS
      op_result = DAA_DAS();
      break;
    case 29: // AAA/AAS
      op_result = AAA_AAS();
      break;
    case 38: // INT 3
      ++reg_ip;
      pc_interrupt(3);
      return true; // no calc ip, no flags
    case 40: // INTO
      ++reg_ip;
      if (FLAG_OF)
        pc_interrupt(4);
      return true; // no calc ip, no flags
    case 41: // AAM;
      if (i_data0 &= 0xFF) {
        regs8[REG_AH] = regs8[REG_AL] / i_data0;
        op_result = regs8[REG_AL] %= i_data0;
      } else {
        // Divide by zero
        raiseDivideByZeroInterrupt();
        return true; // no calc ip, no flags
      }
      break;
    case 42: // AAD
      i_w = 0;
      regs16[REG_AX] = op_result = 0xFF & (regs8[REG_AL] + i_data0 * regs8[REG_AH]);
      break;
    case 43: // SALC
      regs8[REG_AL] = -FLAG_CF;
      break;
    case 45: // CMC
      FLAG_CF ^= 1;
      ++reg_ip;
      return true; // no calc ip, no flags
    case 48: // LOCK:
      break;
    case 49: // HLT
      //printf("CPU HALT, IP = %04X:%04X, AX = %04X, BX = %04X, CX = %04X, DX = %04X\n", regs16[REG_CS], reg_ip, regs16[REG_AX], regs16[REG_BX], regs16[REG_CX], regs16[REG_DX]);
      s_state.halted = true;
      return true; // no calc ip, no flags
    case 51: // 80186, NEC V20: ENTER
    {
      //printf("80186, NEC V20: ENTER\n");
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_BP];
      uint16_t framePtr = regs16[REG_SP];
      int16_t level = i_data2 & 31;
      if (level > 0) {
        while (--level) {
          regs16[REG_BP] -= 2;
          regs16[REG_SP] -= 2;
          MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = MEM16(16 * regs16[REG_SS] + regs16[REG_BP]);
        }
        regs16[REG_SP] -= 2;
        MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = framePtr;
      }
      regs16[REG_BP]  = framePtr;
      regs16[REG_SP] -= i_data0;
      break;
    }
    case 52: // 80186, NEC V20: LEAVE
      //printf("80186, NEC V20: LEAVE\n");
      regs16[REG_SP] = regs16[REG_BP];
      regs16[REG_BP] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      break;
    case 53: // 80186, NEC V20: PUSHA
    {
      //printf("80186, NEC V20: PUSHA\n");
      uint16_t temp = regs16[REG_SP];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_AX];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_CX];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_DX];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_BX];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = temp;
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_BP];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_SI];
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = regs16[REG_DI];
      break;
    }
    case 54: // 80186, NEC V20: POPA
    {
      //printf("80186, NEC V20: POPA\n");
      regs16[REG_DI] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_SI] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_BP] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_SP] += 2;  // SP ignored
      regs16[REG_BX] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_DX] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_CX] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      regs16[REG_AX] = MEM16(16 * regs16[REG_SS] + regs16[REG_SP]);
      regs16[REG_SP] += 2;
      break;
    }
    case 55: // 80186: BOUND
      printf("80186: BOUND - not implemented!\n");
      break;
    case 56: // 80186, NEC V20: PUSH imm16
      //printf("80186, NEC V20: PUSH imm16\n");
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = i_data0;
      break;
    case 57: // 80186, NEC V20: PUSH imm8
      //printf("80186, NEC V20: PUSH imm8\n");
      regs16[REG_SP] -= 2;
      MEM16(16 * regs16[REG_SS] + regs16[REG_SP]) = (i_data0 & 0xff) | (i_data0 & 0x80 ? 0xff00 : 0); // 8->16 bit with sign extension
      break;
    case 58: // 80186 IMUL
      printf("80186 IMUL - not implemented!\n");
      break;
    case 59: // 80186: INSB INSW
      printf("80186: INSB INSW - not implemented!\n");
      break;
    case 60: // 80186: OUTSB OUTSW
      printf("80186: OUTSB OUTSW - not implemented!\n");
      break;
    case 69: // 8087 MATH Coprocessor
      printf("8087 MATH Coprocessor %02X %02X %02X %02X - not implemented!\n", opcode_stream[0], opcode_stream[1], opcode_stream[2], opcode_stream[3]);
      break;
    default:
      printf("Unsupported 8086 opcode %02X %02X\n", opcode_stream[0], opcode_stream[1]);
      break;
  }
  return false;
}


}   // namespace fabgl
