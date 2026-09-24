/*
  EGA planar memory engine -- register file and mask precomputation.

  The hot paths (read8/write8) live inline in ega.h. Everything here runs only when the guest
  touches a Sequencer or Graphics Controller register, which is rare compared to memory access,
  so it can afford to be straightforward and recompute all the derived masks every time.
 */


#include <string.h>

#include "esp_heap_caps.h"

#include "ega.h"


// plane nibble -> one 0xff/0x00 byte lane per plane
const uint32_t EGA::s_expand[16] = {
  0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff,
  0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
  0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
  0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};


EGA::EGA()
{
  memset(m_bank, 0, sizeof(m_bank));
  m_bankMask   = EGA_BANK_COUNT - 1;
  m_offsetMask = EGA_OFFSET_MASK;
  m_oddEven    = false;
  m_mapBase    = 0xa0000;
  m_mapEnd     = 0xb0000;
  reset();
}


EGA::~EGA()
{
  freeVRAM();
}


bool EGA::allocVRAM(int banks)
{
  if (banks != 1 && banks != 2 && banks != 4)
    banks = EGA_BANK_COUNT;

  // release anything above what this mode needs
  for (int i = banks; i < EGA_BANK_COUNT; ++i) {
    if (m_bank[i]) {
      heap_caps_free(m_bank[i]);
      m_bank[i] = nullptr;
    }
  }

  m_bankMask   = (uint32_t)banks - 1;
  m_offsetMask = (uint32_t)banks * EGA_BANK_ENTRIES - 1;

  for (int i = 0; i < banks; ++i) {
    if (!m_bank[i]) {
      m_bank[i] = (uint32_t*) heap_caps_malloc(EGA_BANK_ENTRIES * sizeof(uint32_t),
                                               MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      if (!m_bank[i]) {
        freeVRAM();
        return false;
      }
      memset(m_bank[i], 0, EGA_BANK_ENTRIES * sizeof(uint32_t));
    }
  }
  return true;
}


void EGA::freeVRAM()
{
  for (int i = 0; i < EGA_BANK_COUNT; ++i) {
    if (m_bank[i]) {
      heap_caps_free(m_bank[i]);
      m_bank[i] = nullptr;
    }
  }
}


void EGA::clearVRAM()
{
  for (int i = 0; i < EGA_BANK_COUNT; ++i)
    if (m_bank[i])
      memset(m_bank[i], 0, EGA_BANK_ENTRIES * sizeof(uint32_t));
  m_latch = 0;
}


void EGA::reset()
{
  m_latch    = 0;
  m_seqIndex = 0;
  m_gcIndex  = 0;

  memset(m_seqReg, 0, sizeof(m_seqReg));
  memset(m_gcReg,  0, sizeof(m_gcReg));

  // power-on defaults that matter: all planes writable, all bits writable, nothing forced
  m_seqReg[2] = 0x0f;   // Map Mask: all four planes enabled
  m_gcReg[8]  = 0xff;   // Bit Mask: every bit comes from the CPU

  recomputeMasks();
}


void EGA::writeSequencer(uint8_t value)
{
  m_seqReg[m_seqIndex] = value;
  recomputeMasks();
}


uint8_t EGA::readSequencer()
{
  return m_seqReg[m_seqIndex];
}


void EGA::writeGraphics(uint8_t value)
{
  m_gcReg[m_gcIndex] = value;
  recomputeMasks();
}


uint8_t EGA::readGraphics()
{
  return m_gcReg[m_gcIndex];
}


// The obvious implementation, kept only so the branchless write8() can be checked against it.
void EGA::write8Ref(uint32_t off, uint8_t value)
{
  uint32_t * p   = bankPtr(off);
  uint32_t   lat = m_latch;
  uint32_t   src;

  auto alu = [&](uint32_t s) -> uint32_t {
    switch (m_aluFunc) {
      case 1:  return s & lat;
      case 2:  return s | lat;
      case 3:  return s ^ lat;
      default: return s;
    }
  };

  switch (m_writeMode) {

    case 0:
    {
      uint8_t v = m_rotate ? (uint8_t)((value >> m_rotate) | (value << (8 - m_rotate))) : value;
      src = (uint32_t)v * 0x01010101u;
      src = (src & m_srMask) | m_srBlend;
      src = alu(src);
      src = (src & m_bitMask32) | (lat & ~m_bitMask32);
      break;
    }

    case 1:
      src = lat;
      break;

    case 2:
      src = s_expand[value & 0x0f];
      src = alu(src);
      src = (src & m_bitMask32) | (lat & ~m_bitMask32);
      break;

    default:
    {
      // write mode 3 is VGA only; an EGA treats it as mode 0 with no set/reset
      uint8_t v = m_rotate ? (uint8_t)((value >> m_rotate) | (value << (8 - m_rotate))) : value;
      src = (uint32_t)v * 0x01010101u;
      src = alu(src);
      src = (src & m_bitMask32) | (lat & ~m_bitMask32);
      break;
    }
  }

  *p = (src & m_mapMask32) | (*p & ~m_mapMask32);
}


void EGA::recomputeMasks()
{
  m_mapMask32       = s_expand[m_seqReg[2] & 0x0f];

  m_bitMask32       = (uint32_t)m_gcReg[8] * 0x01010101u;

  uint32_t esr      = s_expand[m_gcReg[1] & 0x0f];      // Enable Set/Reset
  m_srBlend         = s_expand[m_gcReg[0] & 0x0f] & esr; // Set/Reset, only where enabled
  m_srMask          = ~esr;

  m_colorCompare32  = s_expand[m_gcReg[2] & 0x0f];
  m_colorDontCare32 = s_expand[m_gcReg[7] & 0x0f];

  m_rotate          = m_gcReg[3] & 0x07;
  m_aluFunc         = (m_gcReg[3] >> 3) & 0x03;

  m_readMapShift    = (uint8_t)((m_gcReg[4] & 0x03) * 8);

  m_writeMode       = m_gcReg[5] & 0x03;
  m_readMode        = (m_gcReg[5] >> 3) & 0x01;

  // --- fold the mode selectors into masks so the hot path can stay branchless ---

  m_mapMaskInv    = ~m_mapMask32;

  // write mode 1 copies the latches verbatim: an all-zero bit mask makes the general path do that
  m_effBitMask32  = (m_writeMode == 1) ? 0 : m_bitMask32;
  m_effBitMaskInv = ~m_effBitMask32;

  // write mode 2 takes the colour straight from the low nibble instead of the rotated byte
  m_srcSel        = (m_writeMode == 2) ? 0u : 0xffffffffu;
  m_srcSelInv     = ~m_srcSel;

  // write mode 3 does not exist on an EGA and must not pick up Set/Reset
  if (m_writeMode == 3) {
    m_srMask  = 0xffffffffu;
    m_srBlend = 0;
  }

  // Odd/even addressing is on when the Graphics Controller says so and the Sequencer has not
  // disabled it. This is what the BIOS selects for text modes.
  m_oddEven = ((m_gcReg[5] & 0x10) != 0) && ((m_seqReg[4] & 0x04) == 0);

  // Graphics Controller Miscellaneous register, bits 3-2: which CPU window the card answers.
  switch ((m_gcReg[6] >> 2) & 0x03) {
    case 0:  m_mapBase = 0xa0000; m_mapEnd = 0xc0000; break;   // 128K
    case 1:  m_mapBase = 0xa0000; m_mapEnd = 0xb0000; break;   // 64K, graphics modes
    case 2:  m_mapBase = 0xb0000; m_mapEnd = 0xb8000; break;   // 32K, monochrome text
    default: m_mapBase = 0xb8000; m_mapEnd = 0xc0000; break;   // 32K, colour text
  }

  // an unmasked, unmodified write mode 0 is just a splat across the four planes
  m_simpleWrite = (m_writeMode == 0) && (m_rotate == 0) && (m_aluFunc == 0) &&
                  ((m_gcReg[1] & 0x0f) == 0x00) && (m_gcReg[8] == 0xff) &&
                  ((m_seqReg[2] & 0x0f) == 0x0f);

  // ALU as (src & A) ^ (lat & B) ^ (src & lat & C)
  switch (m_aluFunc) {
    case 1:  m_aluA = 0;           m_aluB = 0;           m_aluC = 0xffffffffu; break;  // AND
    case 2:  m_aluA = 0xffffffffu; m_aluB = 0xffffffffu; m_aluC = 0xffffffffu; break;  // OR
    case 3:  m_aluA = 0xffffffffu; m_aluB = 0xffffffffu; m_aluC = 0;           break;  // XOR
    default: m_aluA = 0xffffffffu; m_aluB = 0;           m_aluC = 0;           break;  // replace
  }
}
